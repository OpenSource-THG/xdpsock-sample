# xdpsock-sample

Utility app to test AF_XDP Socket (XSK) infrastructure in the Linux kernel. This work is based
mostly on kernel upstream files in https://github.com/torvalds/linux/tree/master/samples/bpf
specifically xdpsock_user.c/h and xdpsock_kern.c.

Unfortunately the upstream code only tests "multiple XDP sockets" on a single NIC channel,
and so only utilises a single pair of fill/completion queues. This does not show any
synchronisation issues because it's executing in a single thread in both the kernel space
and user space.

For THG's requirements, we need to baseline multi-channel (and thus multi-threaded) performance.
Multi-channel/multi-thread requirements mean we need an XSK setup that uses a dedicated pair
of fill/completion queues per XDP socket to negate the need for synchronisation and locking
around the F/C queues.

Multiple F/C queues are supported - the testing in the upstream kernel repo just doesn't test
it. :( This repo aims to reproduce xdpsock_user/kern code, but with support for multiple fill and
completion queues (referred to in this code as "Multi-FCQ").

## Change 1 - eBPF program

The original code relies on the xdp "dispatcher" if only using 1 XSK, and only uses the
provided xdpsock_kern object if you have "shared UMEM" mode enabled.

This logic is now gated with `USE_ORIGINAL` and turned off by default. We should now always
use our provided xdpsock_kern object.

## Change 2 - Poll mode exit

The original code doesn't properly let you break out (Ctrl-C) of the user app if you use poll
mode. It just hangs - and looks to just be a bug.

This logic is now gated with `USE_ORIGINAL` and we let Ctlr-C work properly.

## Change 3 - Command-line arguments

The original code has two command-line args that we have changed:

 --queue=n

 In the original, this sets the single NIC channel to attach to. If you wanted to run more channels
 you would need to launch multiple instances of the app, but that then has its own UMEM and stuff.

 In the Multi-FCQ build, this is now changed to be the "starting" NIC channel. If we create more
 XSK's, this is incremented by the XSK instance number.

 --shared-umem is now --channels=N

 In the original, this sets "shared umem" mode which created `MAX_SOCKS` XSK sockets that were still
 bound to the single channel specified by --queue=n. `MAX_SOCKS` XSK sockets were created whether you
 wanted them or not.

 In the Multi-FCQ build, this is now changed to be the number of NIC channels to bind to. If we
 specify --queue=1 --channels=4, we'll bind to channels 1-4. (Note channels are indexed from zero!)

 This is the basis for facilitating Multi-FCQ.

## Change 4 - UMEM allocation

In the original code, UMEM allocation size is `NUM_FRAMES * opt_frame_size` [A].

In the Multi-FCQ build, we extend this to be `(NUM_FRAMES * opt_frame_size) * opt_num_xsks` [B].
This is so each XSK socket gets its own area of the UMEM allocation, offset by the XSK index.

The XSK umem offset is calculated via `xsk_index * (NUM_FRAMES * opt_frame_size)` [C] which is stored in the `struct xsk_socket_info` structure. If [A] calculates to 1024, and we have 2 XSKs, then [B] would be calculated at 2048. Thus, [C] for XSK0 would point to 0, and [C] for XSK1 would point to 1024.

This mechanism allows unique descriptor offsets can be calculated for each XSK FQ to maintain thread
safety.

## Change 5 - UMEM fq/cq position

The original code stores fq/cq in `struct xsk_umem_info`. The FQ was filled after UMEM is allocated,
but before any XSK sockets are open.

In the Multi-FCQ build, additioanl fq/cq's are now stored in `struct xsk_socket_info`. The main UMEM FQ is no longer filled before XSK creation, and instead each dedicated XSK FQ is filled once all
XSK's are created.

## Change 6 - XSK mapping to kernel app

The original code inserts XSK FD's into the kernel xsks_map indexed by the XSK index, and the kernel
app simply round-robins through on a packet-by-packet basis.

In the Multi-FCQ build, this logic is amended to insert the XSK FD's into the kernel xsks_map indexed
instead by channel ID, so packets received on a channel can be redirected properly to the respective
XSK bound to that channel.

## Change 7 - Kernel eBPF code

The original eBPF/kernel app just round-robins through `MAX_SOCKS` XSKs and redirects into the xsks_map, whether or not there is an XSK socket actually established.

In the Multi-FCQ build, this logic is amended to instead lookup the `rx_queue_index` instead, and
redirect to the appropriate XSK socket.

In both Multi-FCQ and Single-FCQ builds, we now "lookup" a key before blindly redirecting to one.

## Change 8 - Debug messages in eBPF

The original eBPF/kernel app just does its thing. However, that's quite difficult to observe.

In the revised code, the `odbpf_debug()` macro has come over from OpenDefender so we can print messages to a trace buffer to check what's happening. This macro is enabled in a debug build, and disabled otherwise.

As noted above, we also lookup an xsks_map key first, and log the output, rather than just redirect blindly.

## Change 9 - ARP is passed in eBPF

In the original eBPF/kernel code, ARP is not handled. Fine if you're hard-wired into a port, but not if you're using switches.

In the revised code, we specifically look for ARP packets and XDP_PASS them to the Kernel so switches work. All other traffic is redirected, however.

# How to build

The build.sh script produces a "single FCQ" build and a "multi FCQ" build of both user space app and kernel eBPF code. The script also pulls xdptools and libbpf in and compiles them first.

## Command-line arguments

  --max-xsk N

  Sets `MAX_SOCKS` to N in both user-space and kernel-space code.
  This replaces the hard-defined `MAX_SOCKS 8` in the upstream code.

  In the Single-FCQ build, this means `--shared-umem` will create N sockets whether you want them or not.

  In the Multi-FCQ build, this means `--channels=X` is constrained to (X <= N).

  --debug

  Compiles with `-DUSE_DEBUGMODE=1`.

  In the kernel app, this turns on the `odbpf_debug()` macro for printing trace msgs.
  In the user app, this turns on a warning (because eBPF won't run fast when printing trace msgs!)

  --tar

  Automatically zips compiled objects into xdpsock.tar.gz for SCP to a target server.

  --nodeps

  Disables continual re-compilation of libbpf and libxdp. (For the first build, do not use this).

  --check

  Checks you have some important headers. (Remember to install them.)

## Example

```
[ajm@rocky8dev xdpsock-sample]$ ./build.sh --max-xsk 8 --tar
Checking headers...
 Checking: /usr/include/sys/capability.h
 Checking: /usr/include/pthread.h
Got all headers.
Submodule path 'xdp-tools': checked out '64550eef56d20a03bd081e6d20a4837af3a229c9'
Submodule path 'xdp-tools/lib/libbpf': checked out '533c7666eb728e289abc2e6bf9ecf1186051d7d0'
Reinitialized existing Git repository in /home/ajm/xdpsock-sample/.git/modules/xdptools/
git: 'update' is not a git command. See 'git --help'.

The most similar command is
	update-ref
Found clang binary 'clang' with version 12 (from 'clang version 12.0.1 (Red Hat 12.0.1-4.module+el8.5.0+715+58f51d49)')
libbpf support: submodule vunknown
  perf_buffer__consume support: yes (submodule)
  btf__load_from_kernel_by_id support: yes (submodule)
  btf__type_cnt support: yes (submodule)
  bpf_object__next_map support: yes (submodule)
  bpf_object__next_program support: yes (submodule)
  bpf_program__insn_cnt support: yes (submodule)
  bpf_map_create support: yes (submodule)
  perf_buffer__new_raw support: yes (submodule)
  bpf_xdp_attach support: yes (submodule)
zlib support: yes
ELF support: yes
pcap support: yes
secure_getenv support: yes
gmake: Entering directory '/home/ajm/xdpsock-sample/xdp-tools'

lib

  libbpf
    CC       libbpf/src/libbpf.a
    INSTALL  libbpf/src/libbpf.a

  libxdp

<< libxdp compiled >>
 
gmake: Leaving directory '/home/ajm/xdpsock-sample/xdp-tools'

Running: /usr/bin/gcc -I. -I/home/ajm/xdpsock-sample/xdp-tools/lib/libxdp -I/home/ajm/xdpsock-sample/xdp-tools/lib/libbpf/src/root/usr/include -Wall -g -O2 -DMAX_SOCKS=8 -DMULTI_FCQ=1 -DXDPSOCK_KRNL="xdpsock_multi.bpf"  -o xdpsock_multi xdpsock_user.c /home/ajm/xdpsock-sample/xdp-tools/lib/libxdp/libxdp.a /home/ajm/xdpsock-sample/xdp-tools/lib/libbpf/src/libbpf.a -lcap -pthread -lelf -lz

Compiled successfully: xdpsock_multi


Running: /usr/bin/clang -I. -I/home/ajm/xdpsock-sample/xdp-tools/lib/libbpf/src/root/usr/include -D__KERNEL__ -D__BPF_TRACING__ -DMAX_SOCKS=8 -DMULTI_FCQ=1  -Wall -g -O2 -target bpf -S -emit-llvm xdpsock_kern.c -o xdpsock_kern.ll


Running: /usr/bin/llc -march=bpf -filetype=obj xdpsock_kern.ll -o xdpsock_multi.bpf

Compiled successfully: xdpsock_multi.bpf


Running: /usr/bin/gcc -I. -I/home/ajm/xdpsock-sample/xdp-tools/lib/libxdp -I/home/ajm/xdpsock-sample/xdp-tools/lib/libbpf/src/root/usr/include -Wall -g -O2 -DMAX_SOCKS=8  -DXDPSOCK_KRNL="xdpsock_single.bpf"  -o xdpsock_single xdpsock_user.c /home/ajm/xdpsock-sample/xdp-tools/lib/libxdp/libxdp.a /home/ajm/xdpsock-sample/xdp-tools/lib/libbpf/src/libbpf.a -lcap -pthread -lelf -lz

Compiled successfully: xdpsock_single


Running: /usr/bin/clang -I. -I/home/ajm/xdpsock-sample/xdp-tools/lib/libbpf/src/root/usr/include -D__KERNEL__ -D__BPF_TRACING__ -DMAX_SOCKS=8   -Wall -g -O2 -target bpf -S -emit-llvm xdpsock_kern.c -o xdpsock_kern.ll


Running: /usr/bin/llc -march=bpf -filetype=obj xdpsock_kern.ll -o xdpsock_single.bpf

Compiled successfully: xdpsock_single.bpf


Creating xdpsock.tar.gz from: xdpsock_multi xdpsock_multi.bpf xdpsock_single xdpsock_single.bpf

xdpsock_multi
xdpsock_multi.bpf
xdpsock_single
xdpsock_single.bpf

Tar created: xdpsock.tar.gz

[ajm@rocky8dev xdpsock-sample]$ ls -l xdpsock.tar.gz
-rw-rw-r-- 1 ajm ajm 1397942 Jun 30 22:23 xdpsock.tar.gz
```