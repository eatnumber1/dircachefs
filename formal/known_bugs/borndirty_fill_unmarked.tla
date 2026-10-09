---------------------- MODULE borndirty_fill_unmarked -----------------------
(***************************************************************************)
(* The code today (step 23.11, the audit's G5), without the kernel's lock: *)
(* a lookup of F's name (RecordChild; a listing of D alike) between the    *)
(* create's syscall and its phase 3 inserts F's row, its attributes valid  *)
(* (F's guard was never touched) and no dirty mark, at normal durability   *)
(* (FillMarks <- NoFillMark). Expected: CrashSafe is violated: a power     *)
(* loss may keep that commit and lose the create (and phase 3): a clean    *)
(* row with the attributes of an object the backing filesystem lost,       *)
(* served by nodeid (an NFS handle's LOOKUP(".")).                         *)
(***************************************************************************)
EXTENDS MC
NoFillMark(newRow, dirOk) == FALSE
=============================================================================
