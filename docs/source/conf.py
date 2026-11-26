# sys.path.insert(0, os.path.abspath('..'))

# -- Project information -----------------------------------------------------

project = "Lotus"
copyright = "2024-2026, ZJU Programming Languages and Automated Reasoning Group"
author = "ZJU Programming Languages and Automated Reasoning Group"

# The full version, including alpha/beta/rc tags
release = "v1.0.3"

# General configuration
extensions = [
    'sphinx.ext.autodoc',
    'sphinx.ext.napoleon',
    'sphinx.ext.viewcode',
    'sphinx.ext.githubpages',
    'sphinx_rtd_theme'
]

# Alternative themes you could use instead:
# For alabaster theme (built-in)
# html_theme = 'alabaster'

# For classic theme (built-in)
# html_theme = 'classic'

# For nature theme (built-in)
# html_theme = 'nature'

templates_path = []  # No custom templates directory; keep Sphinx from warning
exclude_patterns = []

# HTML output options
html_theme = 'sphinx_rtd_theme'  # ReadTheDocs theme
html_static_path = ['_static']
html_logo = '_static/logo.svg'
html_title = 'Lotus Documentation'

# LaTeX output options
latex_elements = {
    'papersize': 'letterpaper',
    'pointsize': '10pt',
}