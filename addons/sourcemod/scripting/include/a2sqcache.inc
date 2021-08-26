#if defined _a2sqcache_included
 #endinput
#endif
#define _a2sqcache_included

public Extension __ext_a2sqcache =
{
	name = "a2sqcache",
	file = "a2sqcache.ext",
#if defined AUTOLOAD_EXTENSIONS
	autoload = 1,
#else
	autoload = 0,
#endif
#if defined REQUIRE_EXTENSIONS
	required = 1,
#else
	required = 0,
#endif
};

#if !defined REQUIRE_EXTENSIONS
public __ext_a2sqcache_SetNTVOptional()
{
}
#endif
