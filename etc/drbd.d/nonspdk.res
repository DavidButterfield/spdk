# /etc/drbd.d/nonspdk.res

resource nonspdk {
    on blackbox {
	node-id			0;
	address			192.168.1.22:7789;
	volume 0 {
	    device		drbd1 minor 1;
	    disk		"/UMCfuse/dev/ram000";
	    meta-disk		internal;
	}
    }

    on bottom {
	node-id			2;
	address			192.168.1.23:7789;
	volume 0 {
	    device		drbd1 minor 1;
            disk		"/UMCfuse/dev/file_c";
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
