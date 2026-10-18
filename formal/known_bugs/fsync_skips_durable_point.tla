\* SPDX-License-Identifier: MIT
----------------------- MODULE fsync_skips_durable_point -----------------------
(***************************************************************************)
(* Not the code, and not historical (the 25.28b Opus review's S3): an      *)
(* FSYNC that replies without the cache's durable point                    *)
(* (FsyncSkipsDurablePoint <- Yes; DirCacheFS::Fsync without its           *)
(* MakeCacheDurable), in the implied-directory-fsync program under the     *)
(* ext4 regime (MClitmus.tla). Every invariant still holds: a lost normal  *)
(* commit only costs a re-read. What it breaks is the promise the reply    *)
(* makes, that what dcfs cached so far (an inode row an NFS client's       *)
(* handle names) survives a power loss.                                    *)
(*                                                                         *)
(* Expected: FsyncedCacheDurable is violated: create f (its phase 3 a      *)
(* normal commit), write f, FSYNC of f replies with phase 3's commit       *)
(* still losable.                                                          *)
(***************************************************************************)
EXTENDS MClitmus

Yes == TRUE
=============================================================================
