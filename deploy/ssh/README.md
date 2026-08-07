# SSH deployment

SSH deployment is planned but not executable in this bootstrap. The repository
does not contain host inventories, passwords, private keys, or an implicit
remote command runner.

An executable SSH profile must use an operator-supplied inventory, host-key
verification, least-privilege credentials, explicit target and run IDs, and a
reviewable cleanup plan. It must never copy credentials into run evidence.
