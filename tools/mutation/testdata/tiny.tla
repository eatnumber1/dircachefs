------------------------------ MODULE tiny ------------------------------
(* The fixture of tools/mutation/mutate_test.py: a counter that counts up  *)
(* to 3 and then idles. Its invariant bounds the counter; nothing checks   *)
(* `log`, so a mutant that only changes what is logged survives, and one   *)
(* that lets the counter pass 3 is killed.                                 *)
EXTENDS Naturals

VARIABLES x, log

Init == x = 0 /\ log = 0

Inc ==
    /\ x < 3
    /\ x' = x + 1
    /\ log' = x

Idle ==
    /\ x = 3
    /\ UNCHANGED <<x, log>>

Next == Inc \/ Idle

Bounded == x <= 3
Logged == log <= 3

Spec == Init /\ [][Next]_<<x, log>>
=============================================================================
