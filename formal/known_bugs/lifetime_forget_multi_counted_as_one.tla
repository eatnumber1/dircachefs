---------------- MODULE lifetime_forget_multi_counted_as_one ----------------
(***************************************************************************)
(* A bug the code does not have: each entry of a FORGET_MULTI takes one    *)
(* lookup off dcfs's count instead of the entry's nlookup. dcfs then keeps *)
(* counting lookups the kernel no longer holds, and what it keeps for the  *)
(* nodeid (a removed record, a written_ entry and its held descriptor)     *)
(* outlives the last FORGET.                                               *)
(*                                                                         *)
(* Put in by BugForgetMultiCountsOne. Expected: LookupsExact is violated:  *)
(* two lookups of a; a FORGET_MULTI of both: dcfs counts one left, the     *)
(* kernel none.                                                            *)
(***************************************************************************)
EXTENDS MClifetime
=============================================================================
