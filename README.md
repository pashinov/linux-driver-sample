Linux device driver sample
--------------------------

[![Build Status][travis-badge]][travis-link]

[travis-badge]:    https://travis-ci.org/pashinov/linux-driver-sample.svg?branch=master
[travis-link]:     https://travis-ci.org/pashinov/linux-driver-sample

This linux driver implements linux pipes.

To build the driver:
```
$ make
```

To install the driver:
```
$ sudo insmod pipe.ko
```

To remove the driver:
```
$ sudo rmmod pipe
```

The simple test of driver:
```
$ echo "Hello, World!!!" > /dev/pipe
$ cat /dev/pipe
```

As a result, you will get your message which you sent with command "echo".
