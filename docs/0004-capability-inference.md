# Capability Inference

Compiler analyzes function bodies to infer the weakest required
capability.

Ordering:

read \< write \< take

Public APIs may declare capabilities explicitly. The compiler verifies
implementations satisfy those contracts.
