----------------------------- MODULE MClifetime -----------------------------
(***************************************************************************)
(* The model-checking root module of the lifetime model (lifetime.tla):   *)
(* MC_lifetime*.cfg, known_bugs/lifetime_*.cfg and findings/lifetime_*.cfg *)
(* check this module. README.md, "The lifetime model", says what each     *)
(* checks.                                                                 *)
(***************************************************************************)
EXTENDS lifetime, TLC

CONSTANTS o1, o2, o3, a, b

\* The directory at the start: a and b name o1 and o2; o3 is not created
\* yet (a CREATE or TMPFILE makes it).
MCInitBName == (a :> o1) @@ (b :> o2)
=============================================================================
