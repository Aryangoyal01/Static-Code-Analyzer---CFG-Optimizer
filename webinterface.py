import streamlit as st
import subprocess
import tempfile
import os
import re

st.set_page_config(layout="wide", page_title="C to CFG Optimizer")

st.title(" C Code Static Analyzer & Optimizer")

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
    scanf("%d", &y);
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
            with open("temp.c", "w") as f:
                f.write(c_code)

            try:
                with st.spinner("Running Analysis and Code Rewriting..."):
                    #we Execute the backend here...
                    result = subprocess.run(
                        ["./build/Release/analyzer.exe", "temp.c", "--"], 
                        capture_output=True, 
                        text=True
                    )
                    
                stdout_output = result.stdout
                
                # Tab layout to view both the graph and the new optimized code
                tab1, tab2 = st.tabs([" Visual CFG", " Optimized Code"])
                
                with tab1:
                    # Extract and Render DOT graph using it's inbuilt functionality
                    dot_match = re.search(r'--- COPY BELOW THIS LINE TO A \.DOT FILE ---\n(.*?)\n--- END DOT OUTPUT ---', stdout_output, re.DOTALL)
                    if dot_match:
                        st.graphviz_chart(dot_match.group(1), use_container_width=False)
                    else:
                        st.error("No CFG visualization generated.")
                        
                with tab2:
                    # Read the physically modified file created by the backend Rewriter
                    if os.path.exists("optimized.c"):
                        with open("optimized.c", "r") as opt_file:
                            optimized_code = opt_file.read()
                        st.code(optimized_code, language="c")
                    else:
                        st.info("No optimizations were made, so the code remains unchanged.")
                        st.code(c_code, language="c")

                # Console logs
                with st.expander("View Raw Console Analysis"):
                    st.text(stdout_output)
                    if result.stderr:
                        st.error("Errors:")
                        st.text(result.stderr)

            except Exception as e:
                st.error(f"An error occurred: {e}")
            
            finally:
                # Cleanup
                if os.path.exists("temp.c"):
                    os.remove("temp.c")
                if os.path.exists("optimized.c"):
                    os.remove("optimized.c")