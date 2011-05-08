#include "tcpha_fe_poll.h"

/* Memory cache for epoll items */
struct kmem_cache *tcp_ep_item_cachep = NULL;
atomic_t item_cache_use = ATOMIC_INIT(0);

/* Every socket we are epolling gets one of these linked in to the hash */
struct tcp_ep_item {
	/* Structure Lock, should be got with an IRQ lock
	 * when modifying event_flags and rd_list_link. */
	rwlock_t lock;

	/* The sock this represents */
	struct socket *sock;

	/* Tie us into the RB-Tree hash */
	struct rb_node hash_node;
	
	/* Used to tie this into the ready list */
	struct list_head rd_list_link;

	/* Struct describing the events we are interested in */
	unsigned int event_flags;

	/* Reference count item */
	atomic_t usecnt;
	
	/* The eventpoll this item is tied too */
	struct tcp_eventpoll *eventpoll;

	/* Our wait queue */
	wait_queue_t wait;

	/* Wait queue head we are linked to */
	wait_queue_head_t *whead;
};

/* Private Method Prototypes */
/*---------------------------------------------------------------------------*/
/* Constructor/destructors for our structs */
static int tcp_ep_item_alloc(struct tcp_ep_item **item);
static void tcp_ep_item_free(struct tcp_ep_item *item);
/* Used for memory management, you should always try to free after
 * this. */
static inline void get_ep_item(struct tcp_ep_item *item);

static int inline tcp_epoll_alloc(struct tcp_eventpoll **eventpoll);
static void inline tcp_epoll_free(struct tcp_eventpoll *eventpoll);

/* Utility methods */
/* Called by the socket wakequeue when actvitiy occurs on it, determines if 
 * we should wake any sleepers and add item to readylist */
static int tcp_epoll_wakeup(wait_queue_t *curr, unsigned mode, int sync, void *key);
static inline unsigned int tcp_epoll_check_events(struct tcp_ep_item *item);
static inline struct tcp_ep_item *tcp_ep_item_from_wait(wait_queue_t *p);
static inline void add_item_to_readylist(struct tcp_ep_item *item);

/* RBTree (hash) Usage Methods */
static int tcp_ep_hash_insert(struct tcp_eventpoll *eventpoll, struct tcp_ep_item *item);
static struct tcp_ep_item *tcp_ep_hash_find(struct tcp_eventpoll *eventpoll, struct socket *sock);
//static int tcp_ep_hash_remove(struct tcp_eventpoll *eventpoll, struct socket *);

static inline void tcp_ep_rb_initnode(struct rb_node *n);
static inline void tcp_ep_rb_removenode(struct rb_node *n, struct rb_root *r);
static inline int tcp_cmp_sock(struct socket *leftsock, struct socket *rightsock);

/* Constructor/Destructor methods */
/*---------------------------------------------------------------------------*/
int tcp_epoll_init(struct tcp_eventpoll **eventpoll)
{
	int err;
	struct tcp_eventpoll *ep;

	err = tcp_epoll_alloc(eventpoll);
	if (err)
		return err;
	
	ep = *eventpoll;
	rwlock_init(&ep->lock);
	init_waitqueue_head(&ep->poll_wait);
	INIT_LIST_HEAD(&ep->ready_list);
	rwlock_init(&ep->list_lock);
	ep->hash_root = RB_ROOT;

	/* Guard against multiple initilization, make it for the first user */
	if(atomic_inc_return(&item_cache_use) == 1) {
		tcp_ep_item_cachep = kmem_cache_create("tcp_ep_itemcache", 
										   sizeof(struct tcp_ep_item), 
																	0,
										SLAB_HWCACHE_ALIGN|SLAB_PANIC,
														   NULL, NULL);
	}
	return 0;
}

void tcp_epoll_destroy(struct tcp_eventpoll *eventpoll)
{
	/* Destroy for the last user */
	if (atomic_dec_and_test(&item_cache_use)) {
			kmem_cache_destroy(tcp_ep_item_cachep);
	}
	tcp_epoll_free(eventpoll);
}

static int tcp_epoll_alloc(struct tcp_eventpoll **eventpoll)
{
	struct tcp_eventpoll *ep = kzalloc(sizeof(struct tcp_eventpoll), GFP_KERNEL);

	if (!ep)
		return -ENOMEM;

	*eventpoll = ep;
	return 0;
}

static int tcp_ep_item_alloc(struct tcp_ep_item **item)
{
	struct tcp_ep_item *epi;

	if (!tcp_ep_item_cachep)
		return -1;

	epi = kmem_cache_alloc(tcp_ep_item_cachep, SLAB_KERNEL);

	if(!epi)
		return -ENOMEM;

	rwlock_init(&epi->lock);
	tcp_ep_rb_initnode(&epi->hash_node);
	INIT_LIST_HEAD(&epi->rd_list_link);
	epi->event_flags = 0;
	atomic_set(&epi->usecnt, 1);

	*item = epi;
	return 0;
}

static void tcp_ep_item_free(struct tcp_ep_item *item)
{
	if (item && atomic_dec_and_test(&item->usecnt))
		kmem_cache_free(tcp_ep_item_cachep, item);
}

static inline void get_ep_item(struct tcp_ep_item *item)
{
	atomic_inc(&item->usecnt);
}

static void inline tcp_epoll_free(struct tcp_eventpoll *eventpoll)
{
	if (eventpoll) {
		kfree(eventpoll);
	}
}

/* Modification and Usage Methods */
/*---------------------------------------------------------------------------*/
int tcp_epoll_insert(struct tcp_eventpoll *eventpoll, struct socket *sock, unsigned int flags)
{
	int err;
	unsigned int mask;
	struct tcp_ep_item *item = NULL;

	/* Allocate our item */
	err = tcp_ep_item_alloc(&item);
	if(unlikely(err))
		return err;
	
	/* Setup our item, no need to lock because no one else could POSSIBLY
	 * have it yet. */
	item->sock = sock;
	item->event_flags = flags | POLLERR | POLLHUP;
	item->eventpoll = eventpoll;

	/* And set it up to add itself to the readylist when appropriate */
	init_waitqueue_func_entry(&item->wait, tcp_epoll_wakeup);
	item->whead = sock->sk->sk_sleep;

	/* Add it to the hash*/
	err = tcp_ep_hash_insert(eventpoll, item);
	if (err)
		goto insert_fail;

	/* If its already ready stitch it into the ready list */
	mask = tcp_epoll_check_events(item);
	if (mask)
		add_item_to_readylist(item); /* Locks for us */

	/* Done last so no contention problems */
	add_wait_queue(item->whead, &item->wait);

	return 0;

	/* May occur if the ep goes away while we are trying to insert
	 * an item. */
insert_fail:
	tcp_ep_item_free(item);
	return err;
}

/* Remove the item from all relevant structs etc */
void tcp_epoll_remove(struct tcp_eventpoll *eventpoll, struct socket *sock)
{
	struct tcp_ep_item *item;
	
	/* First find the item for the struct in our RB Tree */
	item = tcp_ep_hash_find(sock);

	/* If we didn't find it... */
	if (!item)
		return;
	
	/* Now remove ourseleves from any poll stuff */
	/* How do I stop race where callback attempts to get lock while
	 * I am killing the item? We use memory management on the item, when
	 * callback starts it "grabs" the item (increasing its ref count),
	 * that way it wont die while its still using it! */

	/* Delete the item from the tree */

	/* Delete the item */
}

int tcp_epoll_setflags(struct tcp_eventpoll *eventpoll, struct socket *sock, unsigned int flags)
{
	/* Find the item, and change its flags */
	return -1;
}

int tcp_epoll_wait(struct tcp_eventpoll *eventpoll, struct socket *sockets[], int maxevents, int timeout)
{
	return -1;
}

/* Private Other Methods */
/*---------------------------------------------------------------------------*/

/* This method REQUIRES to hold the proper locks on item */
static inline unsigned int tcp_epoll_check_events(struct tcp_ep_item *item)
{
	/* Nice little trick, by calling poll without a poll table..
	 * poll returns immediately on tcp sockets (see poll_wait)
	 * and the file is never used :) */
	return item->event_flags & item->sock->ops->poll(NULL, item->sock, NULL);
}

static int tcp_epoll_wakeup(wait_queue_t *curr, unsigned mode, int sync, void *key)
{
//	struct tcp_ep_item *item;
//	int flags;
//	unsigned int mask;
//	struct inet_sock *isk;
	
//	printk(KERN_ALERT "Waking up from socket functionality");
//	item = tcp_ep_item_from_wait(curr);
	/* Unlikely to be held lock so not a big deal, this would only "block"
	 * if we are changing options at the same time on the item or
	 * if this functions is triggered on multiple cpus at once*/
//	isk = inet_sk(item->sock->sk);
//	printk(KERN_ALERT "Got Item: %u.%u.%u.%u", NIPQUAD(isk->daddr));
//	write_lock_irqsave(&item->lock, flags);
//	mask = tcp_epoll_check_events(item);
	/* If the item has events that interest us... */
//	if (mask) {
//		printk(KERN_ALERT "Adding to readylist");
//		add_item_to_readylist(item);
//		printk(KERN_ALERT "Added to readylist");
		/* We may want the below to occur in a workqueue instead  of
		 * in our interrupt context (need to figure out relative cost) */
//		if (waitqueue_active(&item->eventpoll->poll_wait)) {
			/* __wake_up_locked, the epoll version of this isn't exported, hope
			 * this works (looks like an extra lock call.. but w/e */
//			printk(KERN_ALERT "Waking Process");
			//__wake_up(&item->eventpoll->poll_wait, TASK_UNINTERRUPTIBLE | TASK_INTERRUPTIBLE, 1, NULL);
//			printk(KERN_ALERT "Done Waking Process");	
//		}
//	}
//	write_unlock_irqrestore(&item->lock, flags);

//	printk(KERN_ALERT "Done waking stuff");
	unsigned int mask = 0;
	struct tcp_ep_item *item;

	item = tcp_ep_item_from_wait(curr);
	get_ep_item(item);
	mask = tcp_epoll_check_events(item);

	if (mask) {
		printk(KERN_ALERT "Should Wake");
	}

	return 1;
}

static inline struct tcp_ep_item *tcp_ep_item_from_wait(wait_queue_t *p)
{
	return container_of(p, struct tcp_ep_item, wait);
}

/* Item must be properly locked if necessary, rddlist will be lock by method */
static inline void add_item_to_readylist(struct tcp_ep_item *item)
{
	int flags;
	struct tcp_eventpoll *eventpoll = item->eventpoll;
	/* in demand, hold for as short a time as  possible */
	write_lock_irqsave(&eventpoll->list_lock, flags);
	list_add_tail(&item->rd_list_link, &eventpoll->ready_list);
	write_unlock_irqrestore(&eventpoll->list_lock, flags);
}

/* RBTree Methods */
/*---------------------------------------------------------------------------*/
/* All of the blow methods work on the assumption that the caller has done the correct locking */
static int tcp_ep_hash_insert(struct tcp_eventpoll *eventpoll, struct tcp_ep_item *item)
{
	int cmp;
	struct rb_node **p = &eventpoll->hash_root.rb_node;
	struct rb_node *parent = NULL;
	struct tcp_ep_item *epic;

	while (*p) {
		parent = *p;
		epic = rb_entry(parent, struct tcp_ep_item, hash_node);
		cmp = tcp_cmp_sock(epic->sock, item->sock);
		if (cmp == 0)
			return -1;
		p = cmp > 0 ? &parent->rb_right : &parent->rb_left;
	}

	rb_link_node(&item->hash_node, parent, p);
	rb_insert_color(&item->hash_node, &eventpoll->hash_root);

	return 0;
}

static inline void tcp_ep_rb_initnode(struct rb_node *n)
{
	rb_set_parent(n, n);
}

static inline void tcp_ep_rb_removenode(struct rb_node *n, struct rb_root *r)
{
	rb_erase(n, r);
	rb_set_parent(n, n);
}

static struct tcp_ep_item *tcp_ep_hash_find(struct tcp_eventpoll *eventpoll, struct socket *sock)
{
	struct rb_node *rbn;
	struct tcp_ep_item *item = NULL;
	int cmp;
	int found = 0;

	/* Start at root, traverse tree */
	rbn = &eventpoll->hash_root.rb_node;
	while (rbn) {
		item = rb_entry(rbn, struct tcp_ep_item, hash_node);
		cmp = tcp_cmp_sock(sock, item->sock);
		if (cmp == 0) {
			found = 1;
			break;
		}
		rbn = cmp < 0 ? rbn->rb_left : rbn->rb_right;
	}

	/* If not found return null */
	return found ? item : NULL;
}

static inline int tcp_cmp_sock(struct socket *leftsock, struct socket *rightsock)
{
	struct inet_sock *leftsk = inet_sk(leftsock->sk);
	struct inet_sock *rightsk = inet_sk(rightsock->sk);

	/* This compares both the daddr and dport, if both match
	 * the sockets are the same */
	__u32 diff = leftsk->daddr - rightsk->daddr;

	if(!diff)
	{
		diff = leftsk->dport - rightsk->dport;
	}
	
	return diff;
}
