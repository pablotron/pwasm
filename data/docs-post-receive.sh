#!/bin/bash

#
# Post-receive hook for pwasm.org docs repo.
#
# Note: This hook script requires the following:
# * A Python 3 virtual environment with mkdocs configured in
#   ~/venv/mkdocs
# * doxygen installed on the host system
# * the paths below to exist (e.g. $SITE_DIR, etc)
#  

# turn on sane mode
set -eu

# prevent git weirdness
unset GIT_DIR

# set site directory and destination documentation directory
SITE_DIR=~/sites/pwasm.org
DST_DIR="$SITE_DIR/git/docs/site-$(date +%Y%m%d%H%M%S)"

# update git repo
cd "$SITE_DIR/git/docs"
git pull origin master

# activate virtual environment
source "$SITE_DIR/venv/mkdocs/bin/activate"

# update documentation
mkdocs build -d "$DST_DIR"

# build api documentation in "api-docs"
doxygen 

# move api documentation into place
mv api-docs/html "$DST_DIR/api"

# update documentation symlink
rm -f "$SITE_DIR/htdocs/docs/latest"
ln -sf "$DST_DIR" "$SITE_DIR/htdocs/docs/latest"
