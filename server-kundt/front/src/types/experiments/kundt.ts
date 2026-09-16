import type { ESP32Device } from "./esp32";

/*
 * Kundt tube. Three independent ESP32 modules share one logical controller and
 * publish only the fields they own, so every member is optional and the server
 * merges partial payloads instead of replacing them:
 *
 *   E1-Mic       -> mic_amplitude, mic_rms, mic_peak
 *   E2-SineGen   -> frequency, volume
 *   E3-StepMotor -> plunger_actual, clockwise_limit, counter_limit
 */

export interface KundtActuators {
	// AD9833 output frequency, in Hz. The firmware clamps to [20, 20000].
	frequency?: number;

	// Servo angle driving the volume potentiometer, in DEGREES (0-180).
	// Not a percentage, despite what the name suggests.
	volume?: number;

	// Requested plunger position, in cm from the speaker end.
	plunger_pos?: number;

	// One-shot homing request. Never stored server-side: see the DTO note.
	calibrate?: boolean;
}

export interface KundtSensors {
	// Goertzel magnitude at the frequency E2 is driving. This is the
	// measurement; a single DFT bin rejects the stepper, fans and supply ripple.
	mic_amplitude?: number;

	// Broadband RMS. Only useful next to mic_amplitude: their ratio says how
	// much of the level is actually the tone.
	mic_rms?: number;

	// Absolute peak sample of the window, 0..32767. Saturation detector.
	mic_peak?: number;

	plunger_actual?: number;
	clockwise_limit?: boolean;
	counter_limit?: boolean;
}

/*
 * Forma del mensaje que llega por MQTT desde los modulos. NO es la que sale por
 * SSE hacia el navegador.
 */
export interface KundtMQTT {
	device: ESP32Device;
	actuators: KundtActuators;
	sensors: KundtSensors;
}

/*
 * Lo que emite de verdad @Sse('kundt-updates'): la fila entera de Prisma, plana
 * y sin envolver (mqtt-listener.service.ts publica `updated_entry` tal cual).
 * Antes este tipo declaraba {device, actuators, sensors} y el manejador leia
 * data.sensors, que siempre era undefined: la vista jamas se actualizaba.
 */
export type KundtSSE = KundtStatusDTO;

export interface KundtStatusDTO {
	id: number;
	frequency: number;
	volume: number;
	plunger_pos: number;
	plunger_actual: number;
	clockwise_limit: boolean;
	counter_limit: boolean;
	mic_amplitude: number;
	mic_rms: number;
	mic_peak: number;
}

// One point of the amplitude-versus-position sweep the page builds locally.
export interface KundtSample {
	position: number;
	amplitude: number;
}
