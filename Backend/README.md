# BB84 QKD Backend

This project exposes a production-oriented Flask API for simulating the BB84 quantum key distribution protocol with Qiskit. It supports both local execution with `AerSimulator` and remote execution on IBM Quantum backends through `qiskit-ibm-runtime`.

## Features

- Real BB84 state preparation and measurement using Qiskit circuit operations: `reset`, `X`, `H`, and `measure`
- Optional intercept-resend eavesdropper (`Eve`)
- Local testing with `AerSimulator`
- IBM Quantum execution through `QiskitRuntimeService` and `SamplerV2`
- IBM backend discovery through `GET /ibm/backends`
- Clean separation between protocol logic, backend selection, and HTTP API
- Structured logging and JSON error responses

## Install

```powershell
py -m pip install -r requirements.txt
```

## Run

```powershell
py -m bb84_service
```

The service starts on `0.0.0.0:5000` by default.

For a one-command local smoke test, use:

```powershell
.\run_bb84.ps1 -Mode simulator
```

To install dependencies and test both simulator and IBM modes in one flow:

```powershell
.\run_bb84.ps1 -InstallDependencies -Mode both -IbmToken "your-token" -IbmInstance "your-instance"
```

## API

### `GET /bb84`

Query parameters:

- `n`: Number of transmitted qubits. Default: `20`
- `eve`: `true` or `false`. Default: `false`
- `backend`: Optional. `simulator` or `ibm`. Default: `simulator`
- `backend_name`: Optional IBM backend name, such as `ibm_brisbane`

Example:

```powershell
curl "http://localhost:5000/bb84?n=64&eve=true&backend=ibm&backend_name=ibm_brisbane"
```

Response shape:

```json
{
  "data": [
    {
      "alice_bit": 1,
      "alice_basis": "X",
      "bob_basis": "X",
      "bob_bit": 1
    }
  ],
  "error_rate": 0.0,
  "valid_bits": 1,
  "errors": 0,
  "final_key": "1"
}
```

`error_rate > 0.1` should be treated as likely eavesdropping.

The response also includes HTTP headers `X-QKD-Backend-Mode` and `X-QKD-Backend-Name` so a frontend can see which backend actually served the request.

### `GET /ibm/backends`

Returns IBM backend choices for frontend selection.

Query parameters:

- `operational`: Optional. `true` or `false`. Default: `true`
- `simulators`: Optional. `true` or `false`. Default: `false`

Example:

```powershell
curl "http://localhost:5000/ibm/backends?operational=true&simulators=false"
```

## IBM Quantum Setup

The service uses `QiskitRuntimeService` for remote hardware. It can load saved IBM credentials automatically, or you can provide them through environment variables:

```powershell
$env:IBM_QUANTUM_TOKEN="your-token"
$env:IBM_QUANTUM_INSTANCE="your-instance"
$env:IBM_QUANTUM_CHANNEL="ibm_quantum_platform"
```

Optional service settings:

```powershell
$env:BB84_BACKEND_MODE="simulator"
$env:BB84_IBM_BACKEND="ibm_brisbane"
$env:BB84_MAX_TRANSMISSIONS="1024"
$env:BB84_EAVESDROP_THRESHOLD="0.1"
$env:BB84_LOG_LEVEL="INFO"
```

## Protocol Notes

- Alice and Bob choose bases independently for every transmission.
- Eve, when enabled, performs a true intercept-resend attack: she measures the incoming qubit in her chosen basis, then prepares a fresh qubit from that measurement result before Bob receives it.
- `valid_bits` counts only positions where Alice and Bob used the same basis.
- `errors` counts mismatches within those valid positions.
- `final_key` contains only basis-matched bits that agree between Alice and Bob, which gives the frontend a directly usable shared key candidate.
