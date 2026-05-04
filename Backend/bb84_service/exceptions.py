"""Custom exceptions for the BB84 service."""


class BB84Error(Exception):
    """Base exception for the BB84 service."""


class RequestValidationError(BB84Error, ValueError):
    """Raised when an API request contains invalid input."""


class ConfigurationError(BB84Error):
    """Raised when the service configuration is incomplete or invalid."""


class BackendExecutionError(BB84Error):
    """Raised when a quantum backend fails to execute a request."""
