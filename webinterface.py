import streamlit as st
import subprocess
import tempfile
import re
import os
from pathlib import Path

st.set_page_config(layout="wide", page_title="C to CFG Optimizer")

st.title(" C Code Static Analyzer & Optimizer")

PROJECT_DIR = Path(__file__).resolve().parent
ANALYZER_CANDIDATES = [
    PROJECT_DIR / "build" / "analyzer",
    PROJECT_DIR / "analyzer",
    PROJECT_DIR / "build" / "analyzer.exe",
    PROJECT_DIR / "analyzer.exe",
]

def binary_kind(path: Path) -> str:
    try:
        magic = path.read_bytes()[:4]
    except OSError:
        return "unknown"
    if magic == b"\x7fELF":
        return "linux"
    if magic[:2] == b"MZ":
        return "windows"
    return "unknown"

def find_analyzer():
    skipped = []
    for candidate in ANALYZER_CANDIDATES:
        if not candidate.exists():
            continue
        kind = binary_kind(candidate)
        if os.name == "nt" and kind == "linux":
            skipped.append(f"{candidate.name} is a Linux binary, not a Windows executable.")
            continue
        if os.name != "nt" and not os.access(candidate, os.X_OK):
            skipped.append(f"{candidate.name} exists but is not executable.")
            continue
        return candidate, skipped
    return None, skipped

def extract_dot_blocks(stdout_output: str) -> list[str]:
    """
    CFGBuilder.cpp prints exactly one DOT graph between:
    --- COPY BELOW THIS LINE TO A .DOT FILE ---
    ... DOT ...
    --- END DOT OUTPUT ---
    """
    pattern = (
        r'--- COPY BELOW THIS LINE TO A \.DOT FILE ---\r?\n'
        r'(.*?)\r?\n--- END DOT OUTPUT ---'
    )
    dot_graphs = re.findall(pattern, stdout_output, re.DOTALL)
    return [d.strip() for d in dot_graphs if d and d.strip()]

col1, col2 = st.columns(2)

with col1:
    st.header("C Code Input")
    default_code = """#include <stdio.h>
int main() {
    int x = 3 + 5;
    int y;
    while(x > 0) {
        y = 10;
        x = x - 1;
    }
    printf("%d", y);
    return 0;
}

int dead_func() {
    return 42;
}"""
    c_code = st.text_area("Paste your raw C code here:", height=450, value=default_code)
    analyze_btn = st.button("Analyze & Generate CFG", type="primary")

with col2:
    st.header("Output")

    if analyze_btn:
        if c_code.strip() == "":
            st.warning("Please enter some C code to analyze.")
        else:
            analyzer_path, skipped_analyzers = find_analyzer()
            if analyzer_path is None:
                st.error("Analyzer binary not found or not executable. Check permissions or rebuild.")
                st.stop()

            try:
                # Integration requirement:
                # - execute analyzer.exe via subprocess.run
                # - pass source via a temporary file (robust cross-platform)
                # - pass --out to control where rewritten source is written
                with tempfile.TemporaryDirectory() as temp_dir:
                    temp_dir_path = Path(temp_dir)
                    source_path = temp_dir_path / "temp.c"
                    optimized_path = temp_dir_path / "optimized.c"
                    source_path.write_text(c_code, encoding="utf-8")

                    clang_args = [
                        "-xc",
                        "-std=c11",
                        "-I/usr/include",
                        "-I/usr/local/include",
                    ]

                    with st.spinner("Running Analysis and Code Rewriting..."):
                        result = subprocess.run(
                            [
                                str(analyzer_path),
                                "--out",
                                str(optimized_path),
                                "--",
                                *clang_args,
                                str(source_path),
                            ],
                            cwd=temp_dir_path,
                            input=None,
                            capture_output=True,
                            text=True,
                            timeout=60,
                        )

                    stdout_output = result.stdout or ""
                    stderr_output = result.stderr or ""

                    if result.returncode != 0:
                        st.error(f"Compilation/analysis failed (exit code {result.returncode}).")
                        with st.expander("View Analyzer Diagnostics", expanded=True):
                            st.text(stderr_output if stderr_output.strip() else stdout_output)
                        st.stop()

                    tab1, tab2 = st.tabs(["Visual CFG", "Optimized Code"])

                    with tab1:
                        dot_graphs = extract_dot_blocks(stdout_output)
                        if dot_graphs:
                            for i, dot_graph in enumerate(dot_graphs, start=1):
                                st.subheader(f"Function {i}")
                                st.graphviz_chart(dot_graph, use_container_width=True)
                        else:
                            st.warning("No CFG visualization generated (backend produced no DOT output).")

                    with tab2:
                        if optimized_path.exists():
                            optimized_code = optimized_path.read_text(encoding="utf-8", errors="replace")
                            st.code(optimized_code, language="c")
                        else:
                            st.error("Backend did not produce optimized output.")
                            st.code(c_code, language="c")

                    with st.expander("View Raw Console Output"):
                        st.text(stdout_output)
                        if stderr_output.strip():
                            st.text_area("Diagnostics (stderr)", stderr_output, height=220)

            except Exception as e:
                st.error(f"An error occurred: {e}")

