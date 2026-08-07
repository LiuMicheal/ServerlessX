# Local deployment

The local backend is the portable CPU path. It runs inside the checkout,
requires Python 3.9+, and writes only a scoped `runs/<run-id>/` result.

Use `./sx doctor`, `./sx plan --profile cpu`, `./sx run spd --profile cpu`, and
`./sx verify --latest`. There is no local backend in this bootstrap that starts
CUDA workers or a serverless cluster.
