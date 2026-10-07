------------------------------ MODULE MCreval ------------------------------
(***************************************************************************)
(* The model-checking root module of the revalidation model (reval.tla):  *)
(* MC_reval*.cfg, known_bugs/reval_*.cfg and limitations/*.cfg check this *)
(* module. README.md, "The revalidation model", says what each checks.   *)
(***************************************************************************)
EXTENDS reval, TLC

\* dcfs's handles of F are interchangeable (nothing depends on which one an
\* open got), so TLC may treat states that differ only by a permutation of
\* them as one. Safety only: TLC does not support symmetry with liveness.
HandleSymmetry == Permutations(Handles)
=============================================================================
