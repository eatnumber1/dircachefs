------------------- MODULE lifetime_lookup_uncounted -------------------
(***************************************************************************)
(* Not the code (step 12.12a, a premise for ForgetKnown): an entry reply   *)
(* that hands a nodeid to the kernel without counting it in dcfs's        *)
(* lookups_ (as a READDIRPLUS loop without its ++lookups_ would;           *)
(* AfterLookup <- AfterLookupUncounted). Expected: ForgetKnown is          *)
(* violated: the kernel's FORGET of that lookup forgets more than dcfs    *)
(* counted.                                                                *)
(***************************************************************************)
EXTENDS MClifetime
AfterLookupUncounted(s, x) ==
    [s EXCEPT !.k = @ + 1, !.ko = IF s.k = 0 THEN x ELSE @]
=============================================================================
