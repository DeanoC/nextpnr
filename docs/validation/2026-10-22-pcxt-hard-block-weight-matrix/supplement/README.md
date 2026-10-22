# Independent current-capture matrix assertions

All five existing matrix captures have exactly the reference's complete top-level endpoint categories and all ten asserted contract records. This supplements the independent review of the original checker, which did not enumerate candidate-only categories and used an unasserted preservation sentence in its report writer. Neither omission changes these five captured results; this supplement closes those two checks for the current captures only.

The frozen original archive is unchanged: SHA-256 bf676bcf82c9de7484a645ce26fc0b34832066fa8d30458d7f384460628f800c. `current-capture-audit.json` records every category and complete contract-equality assertion per case, with input hashes. `audit.py MATRIX-DIRECTORY REFERENCE-ENDPOINTS OUTPUT-JSON` reproduces the supplement; byte-identical replay passed. With the extracted archive, use its `reference-baseline/endpoints.json`.

This adds no route, hardware result, graph proof or timing acceptance. The matrix remains five failed routes with incomplete coverage; retain the original weight-2000/exponent-5 recipe.
