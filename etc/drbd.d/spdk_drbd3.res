# /etc/drbd.d/spdk_drbd3.res

resource spdk_drbd3 {
    on blackbox {
	node-id			0;
	address			192.168.1.22:7790;
	volume 0 {
	    device		drbd3 minor 3;
	    disk		"/UMCfuse/dev/ram003";
	    meta-disk		internal;
	}
    }

    on bottom {
	node-id			2;
	address			192.168.1.23:7790;
	volume 0 {
	    device		drbd3 minor 3;
            disk		"/UMCfuse/dev/Malloc2";
	    meta-disk		internal;
	 }
    }

#   net {
#	protocol        	A;
#	on-congestion   	pull-ahead;
#	congestion-fill 	1024s; # bytes
#   }

    disk {
	# Put the "c-plan-ahead 0" line first or the others will be silently ignored
	c-plan-ahead		0;			# do not use plan-ahead algorithm
	resync-rate		40000K;			# max bytes/second for resync
	c-min-rate		0k;			# min bytes/second for app
    }
}
