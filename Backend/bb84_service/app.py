"""Flask application exposing the BB84 protocol over HTTP."""

from __future__ import annotations

import logging
import time

from flask import Flask, jsonify, request
from werkzeug.exceptions import HTTPException

from .backends import QiskitExecutor, list_ibm_backends
from .config import ServiceConfig
from .exceptions import BackendExecutionError, ConfigurationError, RequestValidationError
from .protocol import bb84, detect_eavesdropping, generate_key


def create_app(config: ServiceConfig | None = None) -> Flask:
    """Application factory for WSGI servers and local execution."""

    service_config = config or ServiceConfig.from_env()
    _configure_logging(service_config.log_level)

    app = Flask(__name__)
    app.config["SERVICE_CONFIG"] = service_config

    @app.get("/health")
    def health() -> tuple[object, int]:
        return jsonify({"status": "ok"}), 200

    @app.get("/bb84")
    def run_bb84() -> tuple[object, int]:
        started_at = time.perf_counter()
        active_config = app.config["SERVICE_CONFIG"]

        n = _parse_transmissions(
            request.args.get("n"),
            default=20,
            maximum=active_config.max_transmissions,
        )
        eve = _parse_bool(request.args.get("eve"), default=False)
        backend_mode = _parse_backend_mode(
            request.args.get("backend"),
            default=active_config.default_backend_mode,
        )
        backend_name = request.args.get("backend_name") or request.args.get("ibm_backend")

        app.logger.info(
            "Received /bb84 request: n=%s eve=%s backend_mode=%s backend_name=%s",
            n,
            eve,
            backend_mode,
            backend_name or "default",
        )

        active_backend_label = "unknown"
        with QiskitExecutor(
            config=active_config,
            backend_mode=backend_mode,
            backend_name=backend_name,
        ) as executor:
            transmissions = bb84(n=n, eve=eve, executor=executor)
            active_backend_label = executor.backend_label

        stats = detect_eavesdropping(
            transmissions,
            threshold=active_config.eavesdrop_threshold,
        )
        final_key = generate_key(transmissions)

        elapsed_ms = (time.perf_counter() - started_at) * 1000
        app.logger.info(
            "Completed /bb84 request in %.2f ms: valid_bits=%s errors=%s error_rate=%.4f",
            elapsed_ms,
            stats.valid_bits,
            stats.errors,
            stats.error_rate,
        )

        payload = {
            "data": [record.to_dict() for record in transmissions],
            "error_rate": stats.error_rate,
            "valid_bits": stats.valid_bits,
            "errors": stats.errors,
            "final_key": final_key,
        }
        response = jsonify(payload)
        response.headers["X-QKD-Backend-Mode"] = backend_mode
        response.headers["X-QKD-Backend-Name"] = active_backend_label
        return response, 200

    @app.get("/ibm/backends")
    def get_ibm_backends() -> tuple[object, int]:
        active_config = app.config["SERVICE_CONFIG"]
        operational_only = _parse_bool(request.args.get("operational"), default=True)
        include_simulators = _parse_bool(request.args.get("simulators"), default=False)

        backends = list_ibm_backends(
            active_config,
            operational_only=operational_only,
            include_simulators=include_simulators,
        )
        payload = {
            "backends": [backend.to_dict() for backend in backends],
            "default_backend": active_config.default_ibm_backend,
        }
        return jsonify(payload), 200

    @app.errorhandler(RequestValidationError)
    def handle_validation_error(exc: RequestValidationError) -> tuple[object, int]:
        return jsonify({"error": str(exc)}), 400

    @app.errorhandler(ConfigurationError)
    def handle_configuration_error(exc: ConfigurationError) -> tuple[object, int]:
        app.logger.error("Configuration error: %s", exc)
        return jsonify({"error": str(exc)}), 500

    @app.errorhandler(BackendExecutionError)
    def handle_backend_error(exc: BackendExecutionError) -> tuple[object, int]:
        app.logger.error("Backend execution error: %s", exc)
        return jsonify({"error": str(exc)}), 502

    @app.errorhandler(HTTPException)
    def handle_http_error(exc: HTTPException) -> tuple[object, int]:
        return jsonify({"error": exc.description}), exc.code

    @app.errorhandler(Exception)
    def handle_unexpected_error(exc: Exception) -> tuple[object, int]:
        app.logger.exception("Unexpected application error.")
        return jsonify({"error": "Internal server error."}), 500

    return app


def _configure_logging(level_name: str) -> None:
    level = getattr(logging, level_name.upper(), logging.INFO)
    root_logger = logging.getLogger()
    if not root_logger.handlers:
        logging.basicConfig(
            level=level,
            format="%(asctime)s %(levelname)s [%(name)s] %(message)s",
        )
    else:
        root_logger.setLevel(level)

    logging.getLogger("qiskit").setLevel(logging.WARNING)
    logging.getLogger("qiskit_ibm_runtime").setLevel(logging.WARNING)
    logging.getLogger("samplomatic").setLevel(logging.WARNING)


def _parse_transmissions(raw_value: str | None, *, default: int, maximum: int) -> int:
    if raw_value is None:
        return default

    try:
        parsed = int(raw_value)
    except ValueError as exc:
        raise RequestValidationError("Query parameter 'n' must be an integer.") from exc

    if parsed <= 0:
        raise RequestValidationError("Query parameter 'n' must be greater than zero.")
    if parsed > maximum:
        raise RequestValidationError(
            f"Query parameter 'n' exceeds the configured limit of {maximum}."
        )
    return parsed


def _parse_bool(raw_value: str | None, *, default: bool) -> bool:
    if raw_value is None:
        return default

    normalized = raw_value.strip().lower()
    if normalized in {"1", "true", "yes", "on"}:
        return True
    if normalized in {"0", "false", "no", "off"}:
        return False
    raise RequestValidationError(
        "Query parameter 'eve' must be true or false."
    )


def _parse_backend_mode(raw_value: str | None, *, default: str) -> str:
    mode = (raw_value or default).strip().lower()
    if mode not in {"simulator", "ibm"}:
        raise RequestValidationError(
            "Query parameter 'backend' must be either 'simulator' or 'ibm'."
        )
    return mode
