/* cache.c - routines to maintain an in-core cache of entries */
/* $OpenLDAP$ */
/*
 * Copyright 1998-2002 The OpenLDAP Foundation, All Rights Reserved.
 * COPYING RESTRICTIONS APPLY, see COPYRIGHT file
 */

#include "portable.h"

#include <stdio.h>

#include <ac/errno.h>
#include <ac/string.h>
#include <ac/socket.h>

#include "slap.h"

#include "back-ldbm.h"

/* LDBM backend specific entry info -- visible only to the cache */
typedef struct ldbm_entry_info {
	ldap_pvt_thread_rdwr_t	lei_rdwr;	/* reader/writer lock */

	/*
	 * remaining fields require backend cache lock to access
	 * These items are specific to the LDBM backend and should
	 * be hidden.
	 */
	int		lei_state;	/* for the cache */
#define	CACHE_ENTRY_UNDEFINED	0
#define CACHE_ENTRY_CREATING	1
#define CACHE_ENTRY_READY	2
#define CACHE_ENTRY_DELETED	3
#define CACHE_ENTRY_COMMITTED	4
	
	int		lei_refcnt;	/* # threads ref'ing this entry */
	Entry	*lei_lrunext;	/* for cache lru list */
	Entry	*lei_lruprev;
} EntryInfo;
#undef LEI
#define LEI(e)	((EntryInfo *) ((e)->e_private))

static int	cache_delete_entry_internal(Cache *cache, Entry *e);
#ifdef LDAP_DEBUG
static void	lru_print(Cache *cache);
#endif

static int
cache_entry_rdwr_lock(Entry *e, int rw)
{
#ifdef NEW_LOGGING
	LDAP_LOG(( "cache", LDAP_LEVEL_ENTRY,
		   "cache_entry_rdwr_lock: %s lock on ID %ld\n",
		   rw ? "w" : "r", e->e_id ));
#else
	Debug( LDAP_DEBUG_ARGS, "entry_rdwr_%slock: ID: %ld\n",
		rw ? "w" : "r", e->e_id, 0);
#endif


	if (rw)
		return ldap_pvt_thread_rdwr_wlock(&LEI(e)->lei_rdwr);
	else
		return ldap_pvt_thread_rdwr_rlock(&LEI(e)->lei_rdwr);
}

static int
cache_entry_rdwr_trylock(Entry *e, int rw)
{
#ifdef NEW_LOGGING
	LDAP_LOG(( "cache", LDAP_LEVEL_ENTRY,
		   "cache_entry_rdwr_trylock: try %s lock on ID: %ld.\n",
		   rw ? "w" : "r", e->e_id ));
#else
	Debug( LDAP_DEBUG_ARGS, "entry_rdwr_%strylock: ID: %ld\n",
		rw ? "w" : "r", e->e_id, 0);
#endif


	if (rw)
		return ldap_pvt_thread_rdwr_wtrylock(&LEI(e)->lei_rdwr);
	else
		return ldap_pvt_thread_rdwr_rtrylock(&LEI(e)->lei_rdwr);
}

static int
cache_entry_rdwr_unlock(Entry *e, int rw)
{
#ifdef NEW_LOGGING
	LDAP_LOG(( "cache", LDAP_LEVEL_ENTRY,
		   "cache_entry_rdwr_unlock: remove %s lock on ID %ld.\n",
		   rw ? "w" : "r", e->e_id ));
#else
	Debug( LDAP_DEBUG_ARGS, "entry_rdwr_%sunlock: ID: %ld\n",
		rw ? "w" : "r", e->e_id, 0);
#endif


	if (rw)
		return ldap_pvt_thread_rdwr_wunlock(&LEI(e)->lei_rdwr);
	else
		return ldap_pvt_thread_rdwr_runlock(&LEI(e)->lei_rdwr);
}

static int
cache_entry_rdwr_init(Entry *e)
{
	return ldap_pvt_thread_rdwr_init( &LEI(e)->lei_rdwr );
}

static int
cache_entry_rdwr_destroy(Entry *e)
{
	return ldap_pvt_thread_rdwr_destroy( &LEI(e)->lei_rdwr );
}

static int
cache_entry_private_init( Entry*e )
{
	assert( e->e_private == NULL );

	if( e->e_private != NULL ) {
		/* this should never happen */
		return 1;
	}

	e->e_private = ch_calloc(1, sizeof(struct ldbm_entry_info));

	if( cache_entry_rdwr_init( e ) != 0 ) {
		free( LEI(e) );
		e->e_private = NULL;
		return 1;
	} 

	return 0;
}

/*
 * marks an entry in CREATING state as committed, so it is really returned
 * to the cache. Otherwise an entry in CREATING state is removed.
 * Makes e_private be destroyed at the following cache_return_entry_w,
 * but lets the entry untouched (owned by someone else)
 */
void
cache_entry_commit( Entry *e )
{
	assert( e );
	assert( e->e_private );
	assert( LEI(e)->lei_state == CACHE_ENTRY_CREATING );
	/* assert( LEI(e)->lei_refcnt == 1 ); */

	LEI(e)->lei_state = CACHE_ENTRY_COMMITTED;
}

static int
cache_entry_private_destroy( Entry*e )
{
	assert( e->e_private );

	cache_entry_rdwr_destroy( e );

	free( e->e_private );
	e->e_private = NULL;
	return 0;
}

void
cache_return_entry_rw( Cache *cache, Entry *e, int rw )
{
	ID id;
	int refcnt, freeit = 1;

	/* set cache mutex */
	ldap_pvt_thread_mutex_lock( &cache->c_mutex );

	assert( e->e_private );

	cache_entry_rdwr_unlock(e, rw);

	id = e->e_id;
	refcnt = --LEI(e)->lei_refcnt;

	/*
	 * if the entry is returned when in CREATING state, it is deleted
	 * but not freed because it may belong to someone else (do_add,
	 * for instance)
	 */
	if (  LEI(e)->lei_state == CACHE_ENTRY_CREATING ) {
		cache_delete_entry_internal( cache, e );
		freeit = 0;
		/* now the entry is in DELETED state */
	}

	if ( LEI(e)->lei_state == CACHE_ENTRY_COMMITTED ) {
		LEI(e)->lei_state = CACHE_ENTRY_READY;

		/* free cache mutex */
		ldap_pvt_thread_mutex_unlock( &cache->c_mutex );

#ifdef NEW_LOGGING
		LDAP_LOG(( "cache", LDAP_LEVEL_DETAIL1,
			   "cache_return_entry_rw: return (%ld):%s, refcnt=%d\n",
			   id, rw ? "w" : "r", refcnt ));
#else
		Debug( LDAP_DEBUG_TRACE,
			"====> cache_return_entry_%s( %ld ): created (%d)\n",
			rw ? "w" : "r", id, refcnt );
#endif


	} else if ( LEI(e)->lei_state == CACHE_ENTRY_DELETED ) {
		if( refcnt > 0 ) {
			/* free cache mutex */
			ldap_pvt_thread_mutex_unlock( &cache->c_mutex );

#ifdef NEW_LOGGING
			LDAP_LOG(( "cache", LDAP_LEVEL_DETAIL1,
				   "cache_return_entry_rw: %ld, delete pending (%d).\n",
				   id, refcnt ));
#else
			Debug( LDAP_DEBUG_TRACE,
				"====> cache_return_entry_%s( %ld ): delete pending (%d)\n",
				rw ? "w" : "r", id, refcnt );
#endif


		} else {
			cache_entry_private_destroy( e );
			if ( freeit ) {
				entry_free( e );
			}

			/* free cache mutex */
			ldap_pvt_thread_mutex_unlock( &cache->c_mutex );

#ifdef NEW_LOGGING
			LDAP_LOG(( "cache", LDAP_LEVEL_DETAIL1,
				   "cache_return_entry_rw: (%ld): deleted (%d)\n",
				   id, refcnt ));
#else
			Debug( LDAP_DEBUG_TRACE,
				"====> cache_return_entry_%s( %ld ): deleted (%d)\n",
				rw ? "w" : "r", id, refcnt );
#endif

		}

	} else {
		/* free cache mutex */
		ldap_pvt_thread_mutex_unlock( &cache->c_mutex );

#ifdef NEW_LOGGING
		LDAP_LOG(( "cache", LDAP_LEVEL_DETAIL1,
			   "cache_return_entry_rw: ID %ld:%s returned (%d)\n",
			   id, rw ? "w": "r", refcnt ));
#else
		Debug( LDAP_DEBUG_TRACE,
			"====> cache_return_entry_%s( %ld ): returned (%d)\n",
			rw ? "w" : "r", id, refcnt);
#endif

	}
}

#define LRU_DELETE( cache, e ) do { \
	if ( LEI(e)->lei_lruprev != NULL ) { \
		LEI(LEI(e)->lei_lruprev)->lei_lrunext = LEI(e)->lei_lrunext; \
	} else { \
		(cache)->c_lruhead = LEI(e)->lei_lrunext; \
	} \
	if ( LEI(e)->lei_lrunext != NULL ) { \
		LEI(LEI(e)->lei_lrunext)->lei_lruprev = LEI(e)->lei_lruprev; \
	} else { \
		(cache)->c_lrutail = LEI(e)->lei_lruprev; \
	} \
} while(0)

#define LRU_ADD( cache, e ) do { \
	LEI(e)->lei_lrunext = (cache)->c_lruhead; \
	if ( LEI(e)->lei_lrunext != NULL ) { \
		LEI(LEI(e)->lei_lrunext)->lei_lruprev = (e); \
	} \
	(cache)->c_lruhead = (e); \
	LEI(e)->lei_lruprev = NULL; \
	if ( (cache)->c_lrutail == NULL ) { \
		(cache)->c_lrutail = (e); \
	} \
} while(0)

/*
 * cache_add_entry_rw - create and lock an entry in the cache
 * returns:	0	entry has been created and locked
 *		1	entry already existed
 *		-1	something bad happened
 */
int
cache_add_entry_rw(
    Cache	*cache,
    Entry		*e,
	int		rw
)
{
	int	i, rc;
	Entry	*ee;

#ifdef NEW_LOGGING
	LDAP_LOG(( "cache", LDAP_LEVEL_ENTRY,
		   "cache_add_entry_rw: add (%s):%s to cache\n",
		   e->e_dn, rw ? "w" : "r" ));
#endif
	/* set cache mutex */
	ldap_pvt_thread_mutex_lock( &cache->c_mutex );

	assert( e->e_private == NULL );

	if( cache_entry_private_init(e) != 0 ) {
		/* free cache mutex */
		ldap_pvt_thread_mutex_unlock( &cache->c_mutex );

#ifdef NEW_LOGGING
		LDAP_LOG(( "cache", LDAP_LEVEL_ERR,
			   "cache_add_entry_rw: add (%s):%ld private init failed!\n",
			   e->e_dn, e->e_id ));
#else
		Debug( LDAP_DEBUG_ANY,
			"====> cache_add_entry( %ld ): \"%s\": private init failed!\n",
		    e->e_id, e->e_dn, 0 );
#endif


		return( -1 );
	}

	if ( avl_insert( &cache->c_dntree, (caddr_t) e,
		(AVL_CMP) entry_dn_cmp, avl_dup_error ) != 0 )
	{
		/* free cache mutex */
		ldap_pvt_thread_mutex_unlock( &cache->c_mutex );

#ifdef NEW_LOGGING
		LDAP_LOG(( "cache", LDAP_LEVEL_DETAIL1,
			   "cache_add_entry: (%s):%ld already in cache.\n",
			   e->e_dn, e->e_id ));
#else
		Debug( LDAP_DEBUG_TRACE,
			"====> cache_add_entry( %ld ): \"%s\": already in dn cache\n",
		    e->e_id, e->e_dn, 0 );
#endif


		cache_entry_private_destroy(e);

		return( 1 );
	}

	/* id tree */
	if ( avl_insert( &cache->c_idtree, (caddr_t) e,
		(AVL_CMP) entry_id_cmp, avl_dup_error ) != 0 )
	{
#ifdef NEW_LOGGING
		LDAP_LOG(( "cache", LDAP_LEVEL_DETAIL1,
			   "cache_add_entry: (%s):%ls already in cache.\n",
			   e->e_dn, e->e_id ));
#else
		Debug( LDAP_DEBUG_ANY,
			"====> cache_add_entry( %ld ): \"%s\": already in id cache\n",
		    e->e_id, e->e_dn, 0 );
#endif



		/* delete from dn tree inserted above */
		if ( avl_delete( &cache->c_dntree, (caddr_t) e,
			(AVL_CMP) entry_dn_cmp ) == NULL )
		{
#ifdef NEW_LOGGING
			LDAP_LOG(( "cache", LDAP_LEVEL_INFO,
				   "cache_add_entry: can't delete (%s) from cache.\n",
				   e->e_dn ));
#else
			Debug( LDAP_DEBUG_ANY, "====> can't delete from dn cache\n",
			    0, 0, 0 );
#endif

		}

		cache_entry_private_destroy(e);

		/* free cache mutex */
		ldap_pvt_thread_mutex_unlock( &cache->c_mutex );
		return( -1 );
	}

	cache_entry_rdwr_lock( e, rw );

	/* put the entry into 'CREATING' state */
	/* will be marked after when entry is returned */
	LEI(e)->lei_state = CACHE_ENTRY_CREATING;
	LEI(e)->lei_refcnt = 1;

	/* lru */
	LRU_ADD( cache, e );
	if ( ++cache->c_cursize > cache->c_maxsize ) {
		/*
		 * find the lru entry not currently in use and delete it.
		 * in case a lot of entries are in use, only look at the
		 * first 10 on the tail of the list.
		 */
		i = 0;
		while ( cache->c_lrutail != NULL &&
			LEI(cache->c_lrutail)->lei_refcnt != 0 &&
			i < 10 )
		{
			/* move this in-use entry to the front of the q */
			ee = cache->c_lrutail;
			LRU_DELETE( cache, ee );
			LRU_ADD( cache, ee );
			i++;
		}

		/*
		 * found at least one to delete - try to get back under
		 * the max cache size.
		 */
		while ( cache->c_lrutail != NULL &&
			LEI(cache->c_lrutail)->lei_refcnt == 0 &&
			cache->c_cursize > cache->c_maxsize )
		{
			e = cache->c_lrutail;

			/* delete from cache and lru q */
			/* XXX do we need rc ? */
			rc = cache_delete_entry_internal( cache, e );
			cache_entry_private_destroy( e );
			entry_free( e );
		}
	}

	/* free cache mutex */
	ldap_pvt_thread_mutex_unlock( &cache->c_mutex );
	return( 0 );
}

/*
 * cache_update_entry - update a LOCKED entry which has been deleted.
 * returns:	0	entry has been created and locked
 *		1	entry already existed
 *		-1	something bad happened
 */
int
cache_update_entry(
    Cache	*cache,
    Entry		*e
)
{
	int	i, rc;
	Entry	*ee;

	/* set cache mutex */
	ldap_pvt_thread_mutex_lock( &cache->c_mutex );

	assert( e->e_private );

	if ( avl_insert( &cache->c_dntree, (caddr_t) e,
		(AVL_CMP) entry_dn_cmp, avl_dup_error ) != 0 )
	{
#ifdef NEW_LOGGING
		LDAP_LOG(( "cache", LDAP_LEVEL_DETAIL1,
			   "cache_update_entry: (%s):%ld already in dn cache\n",
			   e->e_dn, e->e_id ));
#else
		Debug( LDAP_DEBUG_TRACE,
			"====> cache_update_entry( %ld ): \"%s\": already in dn cache\n",
		    e->e_id, e->e_dn, 0 );
#endif


		/* free cache mutex */
		ldap_pvt_thread_mutex_unlock( &cache->c_mutex );
		return( 1 );
	}

	/* id tree */
	if ( avl_insert( &cache->c_idtree, (caddr_t) e,
		(AVL_CMP) entry_id_cmp, avl_dup_error ) != 0 )
	{
#ifdef NEW_LOGGING
		LDAP_LOG(( "cache", LDAP_LEVEL_DETAIL1,
			   "cache_update_entry: (%s)%ld already in id cache\n",
			   e->e_dn, e->e_id ));
#else
		Debug( LDAP_DEBUG_ANY,
			"====> cache_update_entry( %ld ): \"%s\": already in id cache\n",
		    e->e_id, e->e_dn, 0 );
#endif


		/* delete from dn tree inserted above */
		if ( avl_delete( &cache->c_dntree, (caddr_t) e,
			(AVL_CMP) entry_dn_cmp ) == NULL )
		{
#ifdef NEW_LOGGING
			LDAP_LOG(( "cache", LDAP_LEVEL_INFO,
				   "cache_update_entry: can't delete (%s)%ld from dn cache.\n",
				   e->e_dn, e->e_id ));
#else
			Debug( LDAP_DEBUG_ANY, "====> can't delete from dn cache\n",
			    0, 0, 0 );
#endif

		}

		/* free cache mutex */
		ldap_pvt_thread_mutex_unlock( &cache->c_mutex );
		return( -1 );
	}


	/* put the entry into 'CREATING' state */
	/* will be marked after when entry is returned */
	LEI(e)->lei_state = CACHE_ENTRY_CREATING;

	/* lru */
	LRU_ADD( cache, e );
	if ( ++cache->c_cursize > cache->c_maxsize ) {
		/*
		 * find the lru entry not currently in use and delete it.
		 * in case a lot of entries are in use, only look at the
		 * first 10 on the tail of the list.
		 */
		i = 0;
		while ( cache->c_lrutail != NULL &&
			LEI(cache->c_lrutail)->lei_refcnt != 0 &&
			i < 10 )
		{
			/* move this in-use entry to the front of the q */
			ee = cache->c_lrutail;
			LRU_DELETE( cache, ee );
			LRU_ADD( cache, ee );
			i++;
		}

		/*
		 * found at least one to delete - try to get back under
		 * the max cache size.
		 */
		while ( cache->c_lrutail != NULL &&
			LEI(cache->c_lrutail)->lei_refcnt == 0 &&
			cache->c_cursize > cache->c_maxsize )
		{
			e = cache->c_lrutail;

			/* delete from cache and lru q */
			/* XXX do we need rc ? */
			rc = cache_delete_entry_internal( cache, e );
			cache_entry_private_destroy( e );
			entry_free( e );
		}
	}

	/* free cache mutex */
	ldap_pvt_thread_mutex_unlock( &cache->c_mutex );
	return( 0 );
}

ID
cache_find_entry_ndn2id(
	Backend		*be,
    Cache	*cache,
    struct berval	*ndn
)
{
	Entry		e, *ep;
	ID			id;
	int count = 0;

	/* this function is always called with normalized DN */
	e.e_nname = *ndn;

try_again:
	/* set cache mutex */
	ldap_pvt_thread_mutex_lock( &cache->c_mutex );

	if ( (ep = (Entry *) avl_find( cache->c_dntree, (caddr_t) &e,
		(AVL_CMP) entry_dn_cmp )) != NULL )
	{
		int state;
		count++;

		/*
		 * ep now points to an unlocked entry
		 * we do not need to lock the entry if we only
		 * check the state, refcnt, LRU, and id.
		 */

		assert( ep->e_private );

		/* save id */
		id = ep->e_id;
		state = LEI(ep)->lei_state;

		/*
		 * entry is deleted or not fully created yet
		 */
		if ( state != CACHE_ENTRY_READY ) {
			assert(state != CACHE_ENTRY_UNDEFINED);

			/* free cache mutex */
			ldap_pvt_thread_mutex_unlock( &cache->c_mutex );

#ifdef NEW_LOGGING
			LDAP_LOG(( "cache", LDAP_LEVEL_INFO,
				   "cache_find_entry_dn2id: (%s) %ld not ready: %d\n",
				   ndn->bv_val, id, state ));
#else
			Debug(LDAP_DEBUG_TRACE,
				"====> cache_find_entry_dn2id(\"%s\"): %ld (not ready) %d\n",
				ndn->bv_val, id, state);
#endif


			ldap_pvt_thread_yield();
			goto try_again;
		}

		/* lru */
		LRU_DELETE( cache, ep );
		LRU_ADD( cache, ep );
		
		/* free cache mutex */
		ldap_pvt_thread_mutex_unlock( &cache->c_mutex );

#ifdef NEW_LOGGING
		LDAP_LOG(( "cache", LDAP_LEVEL_DETAIL1,
			   "cache_find_entry_dn2id: (%s): %ld %d tries\n",
			   ndn->bv_val, id, count ));
#else
		Debug(LDAP_DEBUG_TRACE,
			"====> cache_find_entry_dn2id(\"%s\"): %ld (%d tries)\n",
			ndn->bv_val, id, count);
#endif


	} else {
		/* free cache mutex */
		ldap_pvt_thread_mutex_unlock( &cache->c_mutex );

		id = NOID;
	}

	return( id );
}

/*
 * cache_find_entry_id - find an entry in the cache, given id
 */

Entry *
cache_find_entry_id(
	Cache	*cache,
	ID				id,
	int				rw
)
{
	Entry	e;
	Entry	*ep;
	int	count = 0;

	e.e_id = id;

try_again:
	/* set cache mutex */
	ldap_pvt_thread_mutex_lock( &cache->c_mutex );

	if ( (ep = (Entry *) avl_find( cache->c_idtree, (caddr_t) &e,
		(AVL_CMP) entry_id_cmp )) != NULL )
	{
		int state;
		ID	ep_id;

		count++;

		assert( ep->e_private );

		ep_id = ep->e_id; 
		state = LEI(ep)->lei_state;

		/*
		 * entry is deleted or not fully created yet
		 */
		if ( state != CACHE_ENTRY_READY ) {

			assert(state != CACHE_ENTRY_UNDEFINED);

			/* free cache mutex */
			ldap_pvt_thread_mutex_unlock( &cache->c_mutex );

#ifdef NEW_LOGGING
			LDAP_LOG(( "cache", LDAP_LEVEL_INFO,
				   "cache_find_entry_id: (%ld)->%ld not ready (%d).\n",
				   id, ep_id, state ));
				   
#else
			Debug(LDAP_DEBUG_TRACE,
				"====> cache_find_entry_id( %ld ): %ld (not ready) %d\n",
				id, ep_id, state);
#endif


			ldap_pvt_thread_yield();
			goto try_again;
		}

		/* acquire reader lock */
		if ( cache_entry_rdwr_trylock(ep, rw) == LDAP_PVT_THREAD_EBUSY ) {
			/* could not acquire entry lock...
			 * owner cannot free as we have the cache locked.
			 * so, unlock the cache, yield, and try again.
			 */

			/* free cache mutex */
			ldap_pvt_thread_mutex_unlock( &cache->c_mutex );

#ifdef NEW_LOGGING
			LDAP_LOG(( "cache", LDAP_LEVEL_INFO,
				   "cache_find_entry_id: %ld -> %ld (busy) %d.\n",
				   id, ep_id, state ));
#else
			Debug(LDAP_DEBUG_TRACE,
				"====> cache_find_entry_id( %ld ): %ld (busy) %d\n",
				id, ep_id, state);
#endif


			ldap_pvt_thread_yield();
			goto try_again;
		}

		/* lru */
		LRU_DELETE( cache, ep );
		LRU_ADD( cache, ep );
		
		LEI(ep)->lei_refcnt++;

		/* free cache mutex */
		ldap_pvt_thread_mutex_unlock( &cache->c_mutex );

#ifdef NEW_LOGGING
		LDAP_LOG(( "cache", LDAP_LEVEL_DETAIL1,
			   "cache_find_entry_id: %ld -> %s  found %d tries.\n",
			   ep_id, ep->e_dn, count ));
#else
		Debug(LDAP_DEBUG_TRACE,
			"====> cache_find_entry_id( %ld ) \"%s\" (found) (%d tries)\n",
			ep_id, ep->e_dn, count);
#endif


		return( ep );
	}

	/* free cache mutex */
	ldap_pvt_thread_mutex_unlock( &cache->c_mutex );

	return( NULL );
}

/*
 * cache_delete_entry - delete the entry e from the cache.  the caller
 * should have obtained e (increasing its ref count) via a call to one
 * of the cache_find_* routines.  the caller should *not* call the
 * cache_return_entry() routine prior to calling cache_delete_entry().
 * it performs this function.
 *
 * returns:	0	e was deleted ok
 *		1	e was not in the cache
 *		-1	something bad happened
 */
int
cache_delete_entry(
    Cache	*cache,
    Entry		*e
)
{
	int	rc;

	/* set cache mutex */
	ldap_pvt_thread_mutex_lock( &cache->c_mutex );

	assert( e->e_private );

#ifdef NEW_LOGGING
	LDAP_LOG(( "cache", LDAP_LEVEL_ENTRY,
		   "cache_delete_entry: delete %ld.\n", e->e_id ));
#else
	Debug( LDAP_DEBUG_TRACE, "====> cache_delete_entry( %ld )\n",
		e->e_id, 0, 0 );
#endif


	rc = cache_delete_entry_internal( cache, e );

	/* free cache mutex */
	ldap_pvt_thread_mutex_unlock( &cache->c_mutex );
	return( rc );
}

static int
cache_delete_entry_internal(
    Cache	*cache,
    Entry		*e
)
{
	int rc = 0;	/* return code */

	/* dn tree */
	if ( avl_delete( &cache->c_dntree, (caddr_t) e, (AVL_CMP) entry_dn_cmp )
		== NULL )
	{
		rc = -1;
	}

	/* id tree */
	if ( avl_delete( &cache->c_idtree, (caddr_t) e, (AVL_CMP) entry_id_cmp )
		== NULL )
	{
		rc = -1;
	}

	if (rc != 0) {
		return rc;
	}

	/* lru */
	LRU_DELETE( cache, e );
	cache->c_cursize--;

	/*
	 * flag entry to be freed later by a call to cache_return_entry()
	 */
	LEI(e)->lei_state = CACHE_ENTRY_DELETED;

	return( 0 );
}

void
cache_release_all( Cache *cache )
{
	Entry *e;
	int rc;

	/* set cache mutex */
	ldap_pvt_thread_mutex_lock( &cache->c_mutex );

#ifdef NEW_LOGGING
	LDAP_LOG(( "cache", LDAP_LEVEL_ENTRY,
		   "cache_release_all: enter\n" ));
#else
	Debug( LDAP_DEBUG_TRACE, "====> cache_release_all\n", 0, 0, 0 );
#endif


	while ( (e = cache->c_lrutail) != NULL && LEI(e)->lei_refcnt == 0 ) {
#ifdef LDAP_RDWR_DEBUG
		assert(!ldap_pvt_thread_rdwr_active(&LEI(e)->lei_rdwr));
#endif

		/* delete from cache and lru q */
		/* XXX do we need rc ? */
		rc = cache_delete_entry_internal( cache, e );
		cache_entry_private_destroy( e );
		entry_free( e );
	}

	if ( cache->c_cursize ) {
#ifdef NEW_LOGGING
		LDAP_LOG(( "cache", LDAP_LEVEL_INFO,
			   "cache_release_all: Entry cache could not be emptied.\n" ));
#else
		Debug( LDAP_DEBUG_TRACE, "Entry-cache could not be emptied\n", 0, 0, 0 );
#endif

	}

	/* free cache mutex */
	ldap_pvt_thread_mutex_unlock( &cache->c_mutex );
}

#ifdef LDAP_DEBUG

static void
lru_print( Cache *cache )
{
	Entry	*e;

	fprintf( stderr, "LRU queue (head to tail):\n" );
	for ( e = cache->c_lruhead; e != NULL; e = LEI(e)->lei_lrunext ) {
		fprintf( stderr, "\tdn \"%20s\" id %ld refcnt %d\n",
			e->e_dn, e->e_id, LEI(e)->lei_refcnt );
	}
	fprintf( stderr, "LRU queue (tail to head):\n" );
	for ( e = cache->c_lrutail; e != NULL; e = LEI(e)->lei_lruprev ) {
		fprintf( stderr, "\tdn \"%20s\" id %ld refcnt %d\n",
			e->e_dn, e->e_id, LEI(e)->lei_refcnt );
	}
}

#endif

