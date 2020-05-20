#
# Regular cron jobs for the gfxreconstruct package
#
0 4	* * *	root	[ -x /usr/bin/gfxreconstruct_maintenance ] && /usr/bin/gfxreconstruct_maintenance
