Copied from zendfi/zend-devices `arduino/Zend` at commit 16c7c05 (library 0.2.0).
Update by copying the folder again, then re-apply the patch below. The door's
own Zend code is in src/interior/zend_link.cpp.

Local patch (marked `[ATDS patch]`): `ZendEvent::signedAt`, the Unix time from the
signed line, so the door can refuse commands that arrive long after they were sent.
Worth sending upstream.
