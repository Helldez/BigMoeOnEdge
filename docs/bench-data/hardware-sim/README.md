# Simulated machines

Machine specs for `bmoe-planlab simulate` ([../../hardware-simulation.md](../../hardware-simulation.md)),
each composed from the records in [../plan-records](../plan-records/).

- `parts.txt`: published specifications of parts, and which parts each measured machine had (`rig`
  lines): the data the model of parts is fitted on.
- `self-*.machine`: a rented machine rebuilt from its own record, the simulator's check against a
  real fit.
- `one-8gb-*.machine`, `two-16gb-62gb-*.machine`: the two machines of the 0.43 validation that were
  planned and not rented, described by their parts only.
- `check-321-*.machine`: the two-card report of #321, a simulation set against a run someone measured.

The model is passed with `-m`, a shell of the gguf made by `tools/planlab/gguf_shell.py`.
