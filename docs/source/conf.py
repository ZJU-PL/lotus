# sys.path.insert(0, os.path.abspath('..'))

# -- Project information -----------------------------------------------------

project = "Lotus"
copyright = "2024-2025, ZJU Programming Languages and Automated Reasoning Group"
author = "ZJU Programming Languages and Automated Reasoning Group"

# The full version, including alpha/beta/rc tags
release = "v0.3"

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

templates_path = ['_templates']
exclude_patterns = []

# HTML output options
html_theme = 'sphinx_rtd_theme'  # ReadTheDocs theme
# html_static_path = ['_static']
html_title = 'Arlib Documentation'

# LaTeX output options
latex_elements = {
    'papersize': 'letterpaper',
    'pointsize': '10pt',
}