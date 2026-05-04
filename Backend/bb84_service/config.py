"""Runtime configuration for the BB84 service."""

from __future__ import annotations

from dataclasses import dataclass
import os


def _env_bool(name: str, default: bool) -> bool:
    raw_value = os.getenv(name)
    if raw_value is None:
        return default

    normalized = raw_value.strip().lower()
    if normalized in {"1", "true", "yes", "on"}:
        return True
    if normalized in {"0", "false", "no", "off"}:
        return False
    return default


def _env_int(name: str, default: int) -> int:
    raw_value = os.getenv(name)
    if raw_value is None:
        return default
    return int(raw_value)


def _env_float(name: str, default: float) -> float:
    raw_value = os.getenv(name)
    if raw_value is None:
        return default
    return float(raw_value)


@dataclass(frozen=True)
class ServiceConfig:
    """Configuration used by the Flask app and the quantum executor."""

    host: str = "0.0.0.0"
    port: int = 5000
    debug: bool = False
    log_level: str = "INFO"
    default_backend_mode: str = "simulator"
    default_ibm_backend: str | None = None
    ibm_channel: str = "ibm_quantum_platform"
    ibm_instance: str | None = None
    ibm_token: str | None = None
    max_transmissions: int = 1024
    eavesdrop_threshold: float = 0.1
    transpile_optimization_level: int = 1

    @classmethod
    def from_env(cls) -> "ServiceConfig":
        """Load service settings from environment variables."""

        return cls(
            host=os.getenv("BB84_HOST", "0.0.0.0"),
            port=_env_int("BB84_PORT", 5000),
            debug=_env_bool("BB84_DEBUG", False),
            log_level=os.getenv("BB84_LOG_LEVEL", "INFO").upper(),
            default_backend_mode=os.getenv("BB84_BACKEND_MODE", "simulator").strip().lower(),
            default_ibm_backend=os.getenv("BB84_IBM_BACKEND"),
            ibm_channel=os.getenv("IBM_QUANTUM_CHANNEL", "ibm_quantum_platform").strip(),
            ibm_instance=os.getenv("IBM_QUANTUM_INSTANCE"),
            ibm_token=os.getenv("IBM_QUANTUM_TOKEN"),
            max_transmissions=_env_int("BB84_MAX_TRANSMISSIONS", 1024),
            eavesdrop_threshold=_env_float("BB84_EAVESDROP_THRESHOLD", 0.1),
            transpile_optimization_level=_env_int("BB84_OPTIMIZATION_LEVEL", 1),
        )
