"""Quantum backend abstraction used by the BB84 service."""

from __future__ import annotations

from contextlib import AbstractContextManager
from dataclasses import asdict, dataclass
import logging
from typing import Sequence
import warnings

warnings.filterwarnings("ignore", category=UserWarning, module="samplomatic")

from qiskit import QuantumCircuit
from qiskit.transpiler import generate_preset_pass_manager
from qiskit_aer import AerSimulator
from qiskit_ibm_runtime import QiskitRuntimeService, SamplerV2 as Sampler, Session

from .config import ServiceConfig
from .exceptions import BackendExecutionError, ConfigurationError


LOGGER = logging.getLogger(__name__)


@dataclass(frozen=True)
class BackendDescriptor:
    """Serializable summary of an IBM backend."""

    name: str
    operational: bool | None
    simulator: bool | None
    pending_jobs: int | None
    num_qubits: int | None

    def to_dict(self) -> dict[str, str | int | bool | None]:
        return asdict(self)


class QiskitExecutor(AbstractContextManager["QiskitExecutor"]):
    """Execute BB84 circuits on either AerSimulator or IBM Runtime."""

    def __init__(
        self,
        *,
        config: ServiceConfig,
        backend_mode: str | None = None,
        backend_name: str | None = None,
    ) -> None:
        self._config = config
        self._backend_mode = (backend_mode or config.default_backend_mode).strip().lower()
        self._backend_name = backend_name or config.default_ibm_backend
        self._backend = None
        self._pass_manager = None
        self._sampler = None
        self._session_manager: Session | None = None
        self.backend_label = "uninitialized"
        self.backend_mode = self._backend_mode

    def __enter__(self) -> "QiskitExecutor":
        self._backend = self._resolve_backend()
        self._pass_manager = generate_preset_pass_manager(
            backend=self._backend,
            optimization_level=self._config.transpile_optimization_level,
        )

        if self._backend_mode == "ibm":
            self._session_manager = Session(backend=self._backend)
            sampler_mode = self._session_manager.__enter__()
        else:
            sampler_mode = self._backend

        self._sampler = Sampler(mode=sampler_mode)
        self.backend_label = getattr(self._backend, "name", self._backend.__class__.__name__)
        LOGGER.info("Initialized quantum executor with backend %s", self.backend_label)
        return self

    def __exit__(self, exc_type, exc_value, traceback) -> None:
        if self._session_manager is not None:
            self._session_manager.__exit__(exc_type, exc_value, traceback)
        self._session_manager = None
        self._sampler = None
        self._pass_manager = None
        self._backend = None

    def sample_bitstrings(self, circuits: Sequence[QuantumCircuit], *, shots: int = 1) -> list[str]:
        """Run one shot per circuit and return the measured bitstring."""

        if shots != 1:
            raise ValueError("BB84 requires exactly one shot per transmission circuit.")
        if not circuits:
            return []
        if self._sampler is None or self._pass_manager is None:
            raise RuntimeError("Quantum executor must be entered before sampling circuits.")

        try:
            compiled = self._pass_manager.run(list(circuits))
            compiled_circuits = [compiled] if isinstance(compiled, QuantumCircuit) else list(compiled)
            primitive_result = self._sampler.run(compiled_circuits, shots=shots).result()

            bitstrings: list[str] = []
            for index in range(len(compiled_circuits)):
                pub_result = primitive_result[index]
                shot_strings = pub_result.join_data().get_bitstrings()
                if len(shot_strings) != 1:
                    raise BackendExecutionError(
                        f"Expected one measurement result for circuit {index}, received {len(shot_strings)}."
                    )
                bitstrings.append(shot_strings[0])
            return bitstrings
        except BackendExecutionError:
            raise
        except Exception as exc:
            LOGGER.exception("Quantum backend execution failed.")
            raise BackendExecutionError("Quantum backend execution failed.") from exc

    def _resolve_backend(self):
        if self._backend_mode == "simulator":
            return AerSimulator()

        if self._backend_mode != "ibm":
            raise ConfigurationError(
                f"Unsupported backend mode {self._backend_mode!r}. Expected 'simulator' or 'ibm'."
            )

        service = get_ibm_runtime_service(self._config)

        try:
            if self._backend_name:
                return service.backend(self._backend_name)
            return service.least_busy(min_num_qubits=1, operational=True, simulator=False)
        except Exception as exc:
            LOGGER.exception("Unable to resolve an IBM quantum backend.")
            raise ConfigurationError("Unable to resolve an IBM Quantum backend.") from exc


def get_ibm_runtime_service(config: ServiceConfig) -> QiskitRuntimeService:
    """Create an IBM Runtime service instance from the active configuration."""

    service_kwargs: dict[str, str] = {"channel": config.ibm_channel}
    if config.ibm_token:
        service_kwargs["token"] = config.ibm_token
    if config.ibm_instance:
        service_kwargs["instance"] = config.ibm_instance

    try:
        return QiskitRuntimeService(**service_kwargs)
    except Exception as exc:
        LOGGER.exception("Unable to initialize IBM Quantum Runtime service.")
        raise ConfigurationError(
            "Unable to initialize IBM Quantum Runtime. "
            "Verify saved IBM credentials or set IBM_QUANTUM_TOKEN and IBM_QUANTUM_INSTANCE."
        ) from exc


def list_ibm_backends(
    config: ServiceConfig,
    *,
    operational_only: bool = True,
    include_simulators: bool = False,
) -> list[BackendDescriptor]:
    """Return IBM backend metadata that a frontend can use for selection."""

    service = get_ibm_runtime_service(config)

    backend_filters: dict[str, int | bool] = {"min_num_qubits": 1}
    if operational_only:
        backend_filters["operational"] = True
    if not include_simulators:
        backend_filters["simulator"] = False

    try:
        backends = service.backends(**backend_filters)
    except Exception as exc:
        LOGGER.exception("Unable to list IBM quantum backends.")
        raise ConfigurationError("Unable to list IBM Quantum backends.") from exc

    descriptors = [_describe_backend(backend) for backend in backends]
    return sorted(
        descriptors,
        key=lambda item: (
            item.pending_jobs is None,
            item.pending_jobs if item.pending_jobs is not None else 0,
            -(item.num_qubits or 0),
            item.name,
        ),
    )


def _describe_backend(backend) -> BackendDescriptor:
    status = backend.status() if hasattr(backend, "status") else None
    return BackendDescriptor(
        name=getattr(backend, "name", backend.__class__.__name__),
        operational=getattr(status, "operational", getattr(backend, "operational", None)),
        simulator=getattr(backend, "simulator", None),
        pending_jobs=getattr(status, "pending_jobs", None),
        num_qubits=getattr(backend, "num_qubits", None),
    )
