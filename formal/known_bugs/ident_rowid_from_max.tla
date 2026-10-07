------------------------ MODULE ident_rowid_from_max ------------------------
(***************************************************************************)
(* Row ids taken as MAX(id) + 1 (an INTEGER PRIMARY KEY without            *)
(* AUTOINCREMENT) instead of never handing one out twice: once the highest *)
(* row goes, the next row gets its id again, within the same mount, with a *)
(* new generation. The kernel's inode for the old one has the old          *)
(* generation, and fuse_iget marks it bad: its users get EIO (not ESTALE). *)
(* The rows' half of what step 12.4b fixed for stubs (last_stub_id).       *)
(*                                                                         *)
(* Put in by BugRowIdFromMax. Expected: NoBadInode is violated: look a up  *)
(* (row 1); unlink a (its row goes; a removed record answers for it); look *)
(* b up: row 1 again, with another generation, while the kernel holds the  *)
(* inode of row 1.                                                         *)
(***************************************************************************)
EXTENDS MCident
=============================================================================
