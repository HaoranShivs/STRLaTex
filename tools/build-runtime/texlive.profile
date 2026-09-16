# install-tl profile for the PaperForge portable runtime.
# User-mode (no root): every directory lives inside the repository.
# __TEXLIVE_ROOT__ is expanded by build_runtime.sh before the installer runs.
selected_scheme scheme-basic
TEXDIR __TEXLIVE_ROOT__
TEXMFLOCAL __TEXLIVE_ROOT__/texmf-local
TEXMFSYSCONFIG __TEXLIVE_ROOT__/texmf-config
TEXMFSYSVAR __TEXLIVE_ROOT__/texmf-var
TEXMFHOME __TEXLIVE_ROOT__/texmf-home
TEXMFVAR __TEXLIVE_ROOT__/texmf-var
option_doc 0
option_src 0
option_post_code 0
option_adjustrepo 0
