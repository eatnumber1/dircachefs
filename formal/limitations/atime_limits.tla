--------------------------- MODULE atime_limits ---------------------------
(***************************************************************************)
(* What step 23.8 does not deliver, stated as checked counterexamples     *)
(* (README "Limitations", docs/design.md "Access times"):                  *)
(*                                                                         *)
(*  - atime_power_loss_while_open: a power loss after a read of an open    *)
(*    file may leave the cache behind the backing filesystem's access      *)
(*    time: the cold open's dirty row is committed at normal durability    *)
(*    (no WAL fsync per open), so the power loss may take the database     *)
(*    back to before it while the backing filesystem's journal kept the    *)
(*    read's access time. FileExactStrict is violated (FileExact allows    *)
(*    exactly this, and nothing else: never ahead, and never after a      *)
(*    daemon crash).                                                       *)
(*  - dir_atime_cache_only: a directory's (or symlink's) access time is    *)
(*    stamped in the cache on a listing served from it, never on the       *)
(*    backing filesystem, so the cached attributes and the backing         *)
(*    filesystem's differ by it. The real model's D attributes leave the   *)
(*    access time out (README: abstractions); with it in (a readdir that   *)
(*    stamps D's cached attributes with a new value, RDFromStampingAtime), *)
(*    CacheNeverWrong is violated: the comparisons of served and backing  *)
(*    attributes (the fault tests' snapshots) leave directories' and       *)
(*    symlinks' access times out for this reason.                          *)
(***************************************************************************)
EXTENDS MC

RDFromStampingAtime(p, r) ==
    IF DirListable(dbCur) /\ dbCur.attrValid /\ stamp <= MaxStamp
    THEN /\ Serve(ListRes(Listing(dbCur)))
         /\ Reply(p, r, Rep("ok", {ListRes(Listing(dbCur))}))
         /\ Commit([dbCur EXCEPT !.attr = stamp], FALSE)
         /\ stamp' = stamp + 1
         /\ UnchangedBacking /\ UnchangedGuards
    ELSE RDFromCode(p, r)
=============================================================================
