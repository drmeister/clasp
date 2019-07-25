# Usage:
# - do not edit this file
# - create env-local.sh, empty
# - set ONLY those lines you want to change from defaults
#   (system will always first pick up env-default.sh and then
#    override individual lines from env-local.sh)
# - if you want to delete a variable in env-local.sh just set it
#   to the empty string like this:
#   ASDF_OUTPUT_TRANSLATIONS=""

export ASDF_OUTPUT_TRANSLATIONS=/:
export CLASP_QUICKLISP_DIRECTORY=/opt/clasp/lib/clasp/src/lisp/modules/quicklisp

export CLASP_WANT_CANDO=1
# implies CLASP_WANT_CANDO
#export CLASP_WANT_JUPYTER=1

# dev is default
#export CANDO_BRANCH_OR_REVISION=dev