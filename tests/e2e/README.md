# End-to-end tests

These Robot Framework tests run `codec-encode`, `codec-decode` and `codec-analyze` on real files and
check the output files and exit codes. Start them from the root of the repository, after building:

```bash
python3 -m venv .venv
.venv/bin/python -m pip install -r tests/e2e/requirements.txt
.venv/bin/python -m robot --variable TOOLS_DIR:build/default --outputdir build/robot tests/e2e
```

On Windows, the tools are in `build/default/Release` and Python is in `.venv\Scripts`. The helpers in
`AudioLibrary.py` use only the Python standard library.
