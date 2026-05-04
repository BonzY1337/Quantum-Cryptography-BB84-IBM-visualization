"""Entrypoint for running the BB84 service as a module."""

from waitress import serve

from .app import create_app


def main() -> None:
    app = create_app()
    config = app.config["SERVICE_CONFIG"]
    serve(app, host=config.host, port=config.port)


if __name__ == "__main__":
    main()
