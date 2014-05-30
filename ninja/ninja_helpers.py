import subprocess

def shell_escape(str):
    """Escape str such that it's interpreted as a single argument by
the shell."""

    if '"' in str:
        return "'%s'" % str.replace("'", "\\'")
    return str

def cmd_exists(cmd):
    return subprocess.call("type " + cmd, shell=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE) == 0
