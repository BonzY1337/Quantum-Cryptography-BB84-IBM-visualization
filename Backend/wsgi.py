"""WSGI entrypoint for production servers."""

from bb84_service.app import create_app


app = create_app()
