def shell_escape(str):
    """Escape str such that it's interpreted as a single argument by
the shell."""

    if '"' in str:
        return "'%s'" % str.replace("'", "\\'")
    return str
