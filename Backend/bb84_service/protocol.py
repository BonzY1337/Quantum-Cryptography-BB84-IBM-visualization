"""Core BB84 protocol implementation."""

from __future__ import annotations

from dataclasses import asdict, dataclass
import logging
import random
import secrets
from typing import Literal, Protocol, Sequence

from qiskit import ClassicalRegister, QuantumCircuit, QuantumRegister


LOGGER = logging.getLogger(__name__)
Basis = Literal["Z", "X"]


class QuantumExecutor(Protocol):
    """Minimal executor interface needed by the BB84 protocol."""

    backend_label: str

    def sample_bitstrings(self, circuits: Sequence[QuantumCircuit], *, shots: int = 1) -> list[str]:
        """Execute the given circuits and return one bitstring per circuit."""


@dataclass(frozen=True)
class TransmissionPlan:
    """Random choices made by Alice, Bob, and optionally Eve."""

    alice_bit: int
    alice_basis: Basis
    bob_basis: Basis
    eve_basis: Basis | None = None


@dataclass(frozen=True)
class TransmissionRecord:
    """Observable result for one BB84 transmission."""

    alice_bit: int
    alice_basis: Basis
    bob_basis: Basis
    bob_bit: int

    def to_dict(self) -> dict[str, int | str]:
        """Serialize the record for JSON responses."""

        return asdict(self)


@dataclass(frozen=True)
class DetectionStats:
    """Aggregated statistics for eavesdropping detection."""

    valid_bits: int
    errors: int
    error_rate: float
    eavesdropping_detected: bool


def bb84(
    n: int,
    eve: bool,
    executor: QuantumExecutor,
    rng: random.Random | None = None,
) -> list[TransmissionRecord]:
    """Run a BB84 exchange and return Bob's observed outcomes."""

    if n <= 0:
        raise ValueError("n must be a positive integer.")

    bit_source = rng or secrets.SystemRandom()
    plans = [_build_transmission_plan(bit_source, eve=eve) for _ in range(n)]

    LOGGER.info(
        "Running BB84 transmission batch: n=%s eve=%s backend=%s",
        n,
        eve,
        executor.backend_label,
    )

    if eve:
        eve_circuits = [
            _build_measurement_circuit(
                prepared_bit=plan.alice_bit,
                prepared_basis=plan.alice_basis,
                measurement_basis=plan.eve_basis or "Z",
                classical_register_name="eve",
            )
            for plan in plans
        ]
        eve_bitstrings = executor.sample_bitstrings(eve_circuits, shots=1)
        eve_bits = [_extract_single_bit(bitstring) for bitstring in eve_bitstrings]

        bob_circuits = [
            _build_measurement_circuit(
                prepared_bit=eve_bit,
                prepared_basis=plan.eve_basis or "Z",
                measurement_basis=plan.bob_basis,
                classical_register_name="bob",
            )
            for plan, eve_bit in zip(plans, eve_bits)
        ]
    else:
        bob_circuits = [
            _build_measurement_circuit(
                prepared_bit=plan.alice_bit,
                prepared_basis=plan.alice_basis,
                measurement_basis=plan.bob_basis,
                classical_register_name="bob",
            )
            for plan in plans
        ]

    bob_bitstrings = executor.sample_bitstrings(bob_circuits, shots=1)
    bob_bits = [_extract_single_bit(bitstring) for bitstring in bob_bitstrings]

    return [
        TransmissionRecord(
            alice_bit=plan.alice_bit,
            alice_basis=plan.alice_basis,
            bob_basis=plan.bob_basis,
            bob_bit=bob_bit,
        )
        for plan, bob_bit in zip(plans, bob_bits)
    ]


def detect_eavesdropping(
    transmissions: Sequence[TransmissionRecord],
    *,
    threshold: float = 0.1,
) -> DetectionStats:
    """Compare Alice and Bob only where their bases match."""

    valid_records = [
        record
        for record in transmissions
        if record.alice_basis == record.bob_basis
    ]
    errors = sum(record.alice_bit != record.bob_bit for record in valid_records)
    valid_bits = len(valid_records)
    error_rate = (errors / valid_bits) if valid_bits else 0.0

    return DetectionStats(
        valid_bits=valid_bits,
        errors=errors,
        error_rate=error_rate,
        eavesdropping_detected=error_rate > threshold,
    )


def generate_key(transmissions: Sequence[TransmissionRecord]) -> str:
    """Extract the shared sifted key from basis-matched, error-free bits."""

    return "".join(
        str(record.alice_bit)
        for record in transmissions
        if record.alice_basis == record.bob_basis and record.alice_bit == record.bob_bit
    )


def _build_transmission_plan(rng: random.Random, *, eve: bool) -> TransmissionPlan:
    return TransmissionPlan(
        alice_bit=rng.randrange(2),
        alice_basis=_random_basis(rng),
        bob_basis=_random_basis(rng),
        eve_basis=_random_basis(rng) if eve else None,
    )


def _build_measurement_circuit(
    *,
    prepared_bit: int,
    prepared_basis: Basis,
    measurement_basis: Basis,
    classical_register_name: str,
) -> QuantumCircuit:
    """Prepare a BB84 state and measure it in a chosen basis."""

    quantum_register = QuantumRegister(1, "q")
    classical_register = ClassicalRegister(1, classical_register_name)
    circuit = QuantumCircuit(quantum_register, classical_register, name=f"bb84_{classical_register_name}")

    # Start from |0> explicitly so the circuit still models a fresh transmission
    # even if it is reused or embedded into a larger workflow.
    circuit.reset(quantum_register[0])

    # Alice or Eve encodes the classical bit into the selected basis.
    if prepared_bit:
        circuit.x(quantum_register[0])
    if prepared_basis == "X":
        circuit.h(quantum_register[0])

    # Measuring in the X basis is implemented by rotating back into Z first.
    if measurement_basis == "X":
        circuit.h(quantum_register[0])

    circuit.measure(quantum_register[0], classical_register[0])
    return circuit


def _extract_single_bit(bitstring: str) -> int:
    if len(bitstring) != 1 or bitstring not in {"0", "1"}:
        raise ValueError(f"Expected a single-bit measurement outcome, received {bitstring!r}.")
    return int(bitstring)


def _random_basis(rng: random.Random) -> Basis:
    return "X" if rng.randrange(2) else "Z"
