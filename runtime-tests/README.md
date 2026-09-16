# runtime-tests

Fixture documents for the bundled portable TeX Live runtime (plan §19, §20).
They are kept as files so the health check and CI can run them without the
C++ test binary, and so a failing runtime can be reproduced by hand:

```bash
cd /tmp && mkdir t && cd t
cp <repo>/runtime-tests/*.tex .
PATH="<repo>/runtime/texlive/bin/<platform>:$PATH" \
  HOME=<repo>/runtime/texlive \
  latexmk -pdf -interaction=nonstopmode -file-line-error -halt-on-error main.tex
```

## font-test.tex

Regular / bold / italic / bold-italic. Must compile with **no font
substitution** and no "Font shape ... not available" warnings (plan §19).

## ieee-test.tex

IEEEtran conference: title, author, section, the four mark combinations, an
equation (plan §20). IEEEtran pins the body Roman font to ptm (Times) through
its own font-loading path; that selection fails under an XeTeX-based engine,
which is exactly why the templates pin pdfLaTeX. The PDF must contain four
distinct fonts (Regular / Bold / Italic / Bold-Italic); `pdffonts main.pdf`
shows them.
