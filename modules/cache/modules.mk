mod_disk_cache.la: mod_disk_cache.slo
	$(SH_LINK) -rpath $(libexecdir) -module -avoid-version  mod_disk_cache.lo $(MOD_DISK_CACHE_LDADD)
mod_mem_cache.la: mod_mem_cache.slo cache_cache.slo cache_pqueue.slo cache_hash.slo
	$(SH_LINK) -rpath $(libexecdir) -module -avoid-version  mod_mem_cache.lo cache_cache.lo cache_pqueue.lo cache_hash.lo  $(MOD_MEM_CACHE_LDADD)
DISTCLEAN_TARGETS = modules.mk
static = 
shared =  mod_disk_cache.la mod_mem_cache.la
