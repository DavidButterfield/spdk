# /etc/drbd.d/spdk.res

resource spdk {
    on blackbox {
	node-id			0;
	address			192.168.1.22:7788;
	volume 0 {
	    device		drbd2 minor 2;
	    disk		"/UMCfuse/dev/ram001";
	    meta-disk		internal;
	}
    }

    on bottom {
	node-id			2;
	address			192.168.1.23:7788;
	volume 0 {
	    device		drbd2 minor 2;
            disk		"/UMCfuse/dev/ram_b";
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
