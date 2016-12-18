#include "filesys/cache.h"
#include "threads/thread.h"
#include "threads/malloc.h"
#include "devices/timer.h"
#include "filesys/filesys.h"
#include "threads/synch.h"
#include <list.h>
#include <string.h>
#include "filesys/inode.h"

// Used flag for enhanced second change algorithm
enum buf_flag_t {
  B_NOFLAG = 0x0, // 00
  B_RECENT = 0x1, // 01
  B_DIRTY = 0x2, // 10
  B_ALL = 0x3, // 11
};


/* Cache Entry */

struct cache_e
{
  struct lock rw_lock;
  struct lock entry_lock;
  struct condition rw_cond;
  int reader_count;

  bool has_writer;

  block_sector_t sec;

  enum buf_flag_t flag;
  uint8_t data[BLOCK_SECTOR_SIZE];
};



// Only used for element of cache
static struct cache_e cache[MAX_CACHE_SIZE];

static struct lock eviction_lock;
static struct cache_e* clock_cache = cache;
static const struct cache_e* cache_end = cache + MAX_CACHE_SIZE - 1;




/*
 * cache synch functions
 *
 * DESC | lock/unlock implementation with Readers-Writer Algorithm
 *
 */

void cache_write_acquire(struct cache_e* c)
{
  lock_acquire(&c->rw_lock);

  while(c->has_writer ||
      c->reader_count > 0)
    cond_wait(&c->rw_cond, &c->rw_lock);

  c->has_writer = true;

  lock_release(&c->rw_lock);
}

void cache_write_release(struct cache_e* c)
{

  lock_acquire(&c->rw_lock);
  ASSERT(c->has_writer);
  c->has_writer = false;

  cond_broadcast(&c->rw_cond, &c->rw_lock);
  lock_release(&c->rw_lock);
}

void cache_read_acquire(struct cache_e* c)
{
  lock_acquire(&c->rw_lock);

  while(c->has_writer)
    cond_wait(&c->rw_cond, &c->rw_lock);
  c->reader_count++;

  lock_release(&c->rw_lock);
}

void cache_read_release(struct cache_e* c)
{
  lock_acquire(&c->rw_lock);

  ASSERT(c->reader_count > 0);
  if(--c->reader_count == 0)
    cond_signal(&c->rw_cond, &c->rw_lock);

  lock_release(&c->rw_lock);
}


/*
 * cacheWriteBackThread
 *
 * DESC | Write all dirty data per interval. (Flush each clock) 
 *
 * IN   | aux - Dummy NULL pointer
 *
 */
static void cacheWriteBackThread(void* aux UNUSED)
{
  int i;
  while(true){
    timer_sleep(TIMER_FREQ); // TODO: HOW MUCH?

    for(i = 0 ; i < MAX_CACHE_SIZE ; i++) {
      cache_read_acquire(cache + i);
      if(cache[i].flag & B_DIRTY){ 
        block_write(fs_device, cache[i].sec, cache[i].data);
        cache[i].flag -= B_DIRTY;
      }
      cache_read_release(cache + i);
    }
  }
}

/* 
 * cache_init
 *
 * DESC | Initialize cache in first time.
 *
 */
void cache_init()
{
  int i;
  lock_init(&eviction_lock);
  for(i = 0 ; i < MAX_CACHE_SIZE ; i++){
    lock_init(&cache[i].entry_lock); // TODO: It is really needed?
    lock_init(&cache[i].rw_lock);
    cond_init(&cache[i].rw_cond);
    cache[i].reader_count = 0;
    cache[i].sec = -1;
    cache[i].has_writer = false;
    cache[i].flag = B_NOFLAG;
  }
  thread_create("cache_wb", PRI_DEFAULT, cacheWriteBackThread, NULL);
}





/*
 * cacheGetIdx
 *
 * DESC | Get element which has same values with given, update each hit
 *
 * IN   | sec - given sector number
 *      | is_write - if cache_write, true
 *
 * RET  | If fail, evict and get new block, load new one block.
 *      | else, list_elem* of found value
 */

enum write_flag_t {
  W_WRITE,
  W_READ,
  W_NO
};

  static struct cache_e*
cacheGetIdx(block_sector_t sec, enum write_flag_t wflag)
{
  int i;

  for(i = 0 ; i < MAX_CACHE_SIZE ; i++){
    if(cache[i].sec == sec){
      lock_acquire(&cache[i].entry_lock);
      if(cache[i].sec == sec){
        if (wflag == W_WRITE){
          cache_write_acquire(cache + i);
          cache[i].flag |= B_ALL;
        }
        else if(wflag == W_READ)
          cache_read_acquire(cache + i);

        cache[i].flag |= B_RECENT;
        return cache + i;
      }
      else{
        lock_release(&cache[i].entry_lock);
        return NULL;
      }
    }
  }
  return NULL;
}

  static inline void
cacheClockStep(void)
{
  // only one thread can have eviction lock.
  clock_cache = (clock_cache == cache_end) ? cache : clock_cache + 1;
}

  static void
cacheEntryFlush(struct cache_e* e, bool get_entry_lock)
{
  lock_acquire(&e->entry_lock);
  // wait for all writers/readers end
  cache_write_acquire(e);


  ASSERT(e->reader_count == 0);
  ASSERT(e->has_writer);

  if(e->flag & B_DIRTY)
    block_write(fs_device, e->sec, e->data);
  e->sec = -1;
  e->flag = B_RECENT;

  cache_write_release(e);
  if(!get_entry_lock)
    lock_release(&e->entry_lock);
}

  static struct cache_e*
cacheEvict(void)
{
  int try_count = 0;
  const struct cache_e* standard = clock_cache;

  do{
    cacheClockStep();
    // check (0, 0)
    if(!(clock_cache->flag & B_ALL)){
      cacheEntryFlush(clock_cache, true);
      break;
    }

    // if try count >= 1, evict (0, 1) => (1, 0)
    if(try_count >= 1){
      if(clock_cache->flag == B_DIRTY){
        cacheEntryFlush(clock_cache, true);
        break;
      }
      else if(clock_cache->flag & B_RECENT)
        clock_cache->flag -= B_RECENT;
    }

    if(standard == clock_cache)
      try_count++;

  } while(true);


  return clock_cache;
}

/* structure for load thread */

struct ahead_set
{
  block_sector_t sec;
  struct semaphore* sema;
};


/*
 * cacheLoadThread
 *
 * DESC | read-ahead block with thread_create
 *
 * IN   | aux - pointer of ahead_set
 *
 */




static void cacheLoadThread(void* aux)
{
  struct ahead_set aheadWrap = *(struct ahead_set*)aux;

  struct cache_e* ahead;


  if(aheadWrap.sec >= block_size(fs_device)){

    free(aux);
    sema_up(aheadWrap.sema);
    goto done;
  }

  while(!(ahead = cacheGetIdx(aheadWrap.sec, W_NO))){
    if(!lock_try_acquire(&eviction_lock))
      continue;

    ahead = cacheEvict();
    ahead->sec = aheadWrap.sec;

    free(aux);
    sema_up(aheadWrap.sema);

    block_read(fs_device, aheadWrap.sec, ahead->data);
    ahead->flag |= B_RECENT;

    lock_release(&ahead->entry_lock);
    lock_release(&eviction_lock);

    goto done;
  }

  lock_release(&ahead->entry_lock);

  free(aux);
  sema_up(aheadWrap.sema);

done:
  thread_exit();
}

  static struct cache_e*
cacheTryGetIdx(block_sector_t sec, enum write_flag_t wflag)
{
  bool evicted = false;
  struct cache_e* item;
  while(!(item = cacheGetIdx(sec, wflag))){
    // only lock held by this thread
    if(!lock_try_acquire(&eviction_lock))
      continue;

    evicted = true;

    // load now block
    item = cacheEvict();

    item->sec = sec;
    block_read(fs_device, sec, item->data);

    item->flag |= B_RECENT;

    lock_release(&item->entry_lock);
    lock_release(&eviction_lock);



    // get r/w cond in here
    if (wflag == W_WRITE){
      cache_write_acquire(item);
      item->flag |= B_ALL;
    }
    else if(wflag == W_READ)
      cache_read_acquire(item);


    // load second block

    struct semaphore sema1;
    sema_init(&sema1, 0);
    struct ahead_set* aheadWrap = malloc(sizeof(struct ahead_set));
    aheadWrap->sec = sec + 1;
    aheadWrap->sema = &sema1;


    thread_create("ahead_reader", PRI_DEFAULT, cacheLoadThread, aheadWrap);
    sema_down(&sema1);

    break;
  }
  if(!evicted)
    lock_release(&item->entry_lock);
  return item;
}





/* NOTE:
 * every function that use read/write function will take 
 * cache_e size >= BLOCK_SECTOR_SIZE with bounce.
 * Do not have to care about cache_e size. 
 */


/*
 * cache_write
 *
 * DESC | Write Data 'from' and other extra field to valid cache entry.
 *      | Then, Update Cache(MRU), mark DIRTY flag.
 *
 * IN   | sec - given sector number
 *      | from - caller's data
 *
 */
void cache_write(block_sector_t sec, const void* from)
{
  struct cache_e* buffer = cacheTryGetIdx(sec, W_WRITE);

  // get lock by cacheGetIdx or cacheLoadBlock
  memcpy(buffer->data, from, BLOCK_SECTOR_SIZE);
  cache_write_release(buffer);
}


/*
 * cache_read
 *
 * DESC | Write cache to Data 'to', and fill extra field to valid cache entry.
 *      | Then, Update Cache(MRU).
 *
 * IN   | sec - given sector number
 *      | to - caller's data
 *
 */
void cache_read(block_sector_t sec, void* to)
{
  struct cache_e* buffer = cacheTryGetIdx(sec, W_READ);

  memcpy(to, buffer->data, BLOCK_SECTOR_SIZE);
  cache_read_release(buffer);
}



/*
 * cache_flush
 *
 * DESC | clear all cache (If dirty, write-back)
 *
 */
void cache_flush(void)
{
  int i;
  for(i = 0 ; i < MAX_CACHE_SIZE ; i++){

    if(cache[i].flag & B_DIRTY){
      block_write(fs_device, cache[i].sec, cache[i].data);
      cache[i].flag -= B_DIRTY;
    }
  }
}



