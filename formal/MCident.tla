------------------------------ MODULE MCident ------------------------------
(***************************************************************************)
(* The model-checking root module of the identity model (ident.tla):      *)
(* MC_ident*.cfg, known_bugs/ident_*.cfg and limitations/ident_*.cfg      *)
(* check this module. README.md, "The identity model", says what each     *)
(* checks.                                                                 *)
(***************************************************************************)
EXTENDS ident, TLC

CONSTANTS o1, o2, o3, m1, m2, a, b

\* The directory at the start: a and b name o1 and o2; o3 is not created
\* yet, and gets o1's inode number (a recycling) once o1 is freed. m1 and
\* m2 are filesystems that may be mounted at a name (boundaries).
MCInitBName == (a :> o1) @@ (b :> o2)
\* For the stubs: a filesystem mounted at each name.
MCInitBNameStubs == (a :> m1) @@ (b :> m2)
MCInoOf == (o1 :> 1) @@ (o2 :> 2) @@ (o3 :> 1) @@ (m1 :> 0) @@ (m2 :> 0)
MCGenOf == (o1 :> 1) @@ (o2 :> 2) @@ (o3 :> 3) @@ (m1 :> 4) @@ (m2 :> 5)

\* Every kind of evidence ext4, xfs and btrfs give for a regular file or a
\* directory.
AllEvidence == {"handle_gen", "getversion", "btime"}
=============================================================================
