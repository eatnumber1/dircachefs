\* SPDX-License-Identifier: MIT
--------------- MODULE durability_fsync_skips_durable_point ---------------
(***************************************************************************)
(* Not the code, and not historical (the 25.28b Opus review's S3): an      *)
(* FSYNC that replies without the cache's durable point (DirCacheFS::Fsync *)
(* without its MakeCacheDurable). What dcfs cached about the file, the     *)
(* inode row an NFS client's handle names among it, stays losable after    *)
(* the caller was told it is durable.                                      *)
(*                                                                         *)
(* Re-introduced by BugFsyncSkipsDurablePoint. Expected: FsyncedDurable is *)
(* violated: a fill, a tick, a request whose FSYNC replies with the fill   *)
(* still losable.                                                          *)
(***************************************************************************)
EXTENDS durability
=============================================================================
