import { Alert, Box, Card, CardContent, Chip, Grid, InputAdornment, LinearProgress, Slider, Stack, TextField, Typography, useTheme } from "@mui/material";
import { useEffect, useMemo, useRef, useState } from "react";
import { useLoaderData, useParams } from "react-router";
import { ResponsiveLine } from "@nivo/line";

import CamerasDisplay from "../../../components/cameraviewer/CamerasDisplay";
import { ButtonCB } from "../../../components/buttons/button";
import { CardTitle } from "../../../components/misc/Title";
import { useSnackbar } from "../../../components/toast/ToastProvider";
import UserGrid from "../../../components/user-grid/UserGrid";
import generatePlotTheme from "../PlotTheme";
import { CONTROL_BORDER_CARD_RAIDUS } from "../Constants";

import { getCameraList } from "../../../api/camera";
import { updateKundt } from "../../../api/experiments/kundt";
import { startAuthenticatedSSE } from "../../../api/experiments/sse";
import { SSE_KUNDT_UPDATES } from "../../../api/routes";

import type { CameraOBJ } from "../../../types/cameras";
import type { KundtSample, KundtSensors, KundtSSE, KundtStatusDTO } from "../../../types/experiments/kundt";

import GraphicEqIcon from "@mui/icons-material/GraphicEq";
import VolumeUpIcon from "@mui/icons-material/VolumeUp";
import StraightenIcon from "@mui/icons-material/Straighten";
import HomeIcon from "@mui/icons-material/Home";
import PlayArrowIcon from "@mui/icons-material/PlayArrow";
import StopIcon from "@mui/icons-material/Stop";
import DeleteSweepIcon from "@mui/icons-material/DeleteSweep";

/* Firmware limits, mirrored here so the UI rejects before the round trip. */
const FREQ_MIN_HZ = 20;
const FREQ_MAX_HZ = 20000;
const SERVO_MAX_DEG = 180;

/* mic_peak is a 16-bit magnitude. Past this the analog front end is clipping
 * and the amplitude reading stops being trustworthy. */
const PEAK_FULL_SCALE = 32767;
const PEAK_SATURATION = 31000;

/* Below this ratio the microphone is hearing room noise, not the tone. */
const TONE_RATIO_THRESHOLD = 50;

/* Speed of sound at 20 C, only used to label the expected spacing. */
const REFERENCE_SPEED_MS = 343;

const MAX_SAMPLES = 600;

/*
 * Margen antes de dar por caida la conexion. fetchEventSource reintenta solo y
 * un reintento normal tarda ~1 s; avisar antes de eso convierte cada hipo de red
 * y cada cambio de pestaña en una alarma falsa.
 */
const LINK_GRACE_MS = 4000;

/* Por encima de esto los datos en pantalla dejan de considerarse frescos. */
const STALE_MS = 3000;

/*
 * Camaras montadas fisicamente del reves. Es un apaño de presentacion: el
 * arreglo de fondo es set_vflip() en el firmware de esa placa, que corrige la
 * imagen en origen y vale tambien para go2rtc y para cualquier otro consumidor.
 * Requiere acceso USB a la camara, asi que mientras tanto se corrige aqui.
 */
const FLIPPED_CAMERAS = ["kundt1-cam3"];

/*
 * Recorrido util del riel, en cm desde el parlante. Son las posiciones de los
 * dos fines de carrera en el firmware de E3 (stepper_math.h): fuera de ahi el
 * embolo no puede ir, asi que la barra no debe ofrecerlo.
 */
const PLUNGER_MIN_CM = 26;
const PLUNGER_MAX_CM = 84;

/*
 * La barra de frecuencia es logaritmica. En lineal, 20 Hz a 20 kHz deja todo
 * el rango util del experimento (unos cientos de Hz a unos pocos kHz) apretado
 * en el primer 10% del recorrido, y un pixel vale 200 Hz.
 */
const freqToSlider = (hz: number) =>
    100 * Math.log(hz / FREQ_MIN_HZ) / Math.log(FREQ_MAX_HZ / FREQ_MIN_HZ);
const sliderToFreq = (pos: number) =>
    Math.round(FREQ_MIN_HZ * Math.pow(FREQ_MAX_HZ / FREQ_MIN_HZ, pos / 100));

const FREQ_MARKS = [100, 1000, 10000].map((hz) => ({
    value: freqToSlider(hz),
    label: hz >= 1000 ? `${hz / 1000}k` : String(hz),
}));

/*
 * La barra del embolo va invertida: 84 cm a la izquierda y 26 a la derecha, para
 * que la barra se mueva en el mismo sentido que el embolo en el montaje.
 *
 * Se consigue trabajando en escala negada (-84 .. -26) en vez de con un
 * transform CSS: scaleX(-1) voltearia tambien las etiquetas de las marcas y los
 * numeros saldrian espejados. Aqui solo se niega el valor al entrar y al salir,
 * y todo lo que se lee en pantalla sigue en centimetros normales.
 */
const toSliderCm = (cm: number) => -cm;
const fromSliderCm = (value: number) => -value;

const PLUNGER_MARKS = [26, 40, 55, 70, 84].map((cm) => ({ value: toSliderCm(cm), label: String(cm) }));

const VOLUME_MARKS = [
    { value: 0, label: "0" },
    { value: 45, label: "45" },
    { value: 90, label: "90" },
    { value: 135, label: "135" },
    { value: 180, label: "180" },
];

/*
 * Resonance finder.
 *
 * Consecutive maxima of the standing wave sit half a wavelength apart, so the
 * mean spacing between peaks gives v = 2 * spacing * frequency. Peaks are taken
 * as samples above 60% of the sweep's range that also dominate their
 * neighbourhood, which is enough for a hand-driven sweep and avoids reporting
 * every ripple as a resonance.
 */
function findResonances(samples: KundtSample[]): number[] {
    if (samples.length < 12) return [];

    const sorted = [...samples].sort((a, b) => a.position - b.position);
    const amplitudes = sorted.map((s) => s.amplitude);
    const lo = Math.min(...amplitudes);
    const hi = Math.max(...amplitudes);
    if (hi <= lo) return [];

    const threshold = lo + (hi - lo) * 0.6;
    const window = 3;
    const peaks: number[] = [];

    for (let i = window; i < sorted.length - window; i++) {
        const here = sorted[i].amplitude;
        if (here < threshold) continue;

        let dominates = true;
        for (let k = i - window; k <= i + window; k++) {
            if (k !== i && sorted[k].amplitude > here) { dominates = false; break; }
        }
        if (!dominates) continue;

        /* Collapse peaks closer than 2 cm: the same maximum sampled twice. */
        const position = sorted[i].position;
        if (peaks.length > 0 && Math.abs(position - peaks[peaks.length - 1]) < 2) continue;
        peaks.push(position);
    }

    return peaks;
}

function speedFromResonances(peaks: number[], frequency: number): number | null {
    if (peaks.length < 2 || frequency <= 0) return null;

    let total = 0;
    for (let i = 1; i < peaks.length; i++) total += peaks[i] - peaks[i - 1];
    const mean_spacing_cm = total / (peaks.length - 1);

    /* spacing is half a wavelength; cm -> m */
    return 2 * (mean_spacing_cm / 100) * frequency;
}

export default function Kundt() {
    const initial = useLoaderData() as KundtStatusDTO;
    const { id } = useParams();
    const platform_id = Number(id);
    const theme = useTheme();
    const { showSnackBar } = useSnackbar();

    const [cameras, setCameras] = useState<CameraOBJ[]>([]);
    const [sensors, setSensors] = useState<KundtSensors>({
        mic_amplitude: initial.mic_amplitude,
        mic_rms: initial.mic_rms,
        mic_peak: initial.mic_peak,
        plunger_actual: initial.plunger_actual,
        clockwise_limit: initial.clockwise_limit,
        counter_limit: initial.counter_limit,
    });

    const [frequency, setFrequency] = useState<number>(initial.frequency);

    /*
     * Valor que sigue al dedo mientras se arrastra. La consigna se publica solo
     * al soltar (onChangeCommitted): con onChange cada pixel del recorrido seria
     * un mensaje MQTT y una escritura en PostgreSQL.
     */
    /*
     * Mientras se arrastra, el SSE no debe mover la barra bajo el dedo: el
     * servidor sigue emitiendo la consigna vieja hasta que soltamos y llega la
     * nueva, asi que sincronizar en ese momento la haria saltar hacia atras.
     */
    const draggingRef = useRef(false);

    const [freqDraft, setFreqDraft] = useState<number>(initial.frequency);
    /* Texto de la caja de frecuencia. Va aparte del numero porque mientras se
     * escribe hay estados intermedios que no son una frecuencia valida ("", "1",
     * "12"): convertirlos en consigna mandaria al DDS a 1 Hz de paso. */
    const [freqText, setFreqText] = useState<string>(String(initial.frequency));
    const [volumeDraft, setVolumeDraft] = useState<number>(initial.volume);
    const [plungerDraft, setPlungerDraft] = useState<number>(initial.plunger_pos);

    const [samples, setSamples] = useState<KundtSample[]>([]);

    /* Estado del enlace y edad del ultimo dato, para que la vista diga si lo que
     * muestra es de ahora o quedo congelado. */
    const [linkUp, setLinkUp] = useState<boolean>(true);
    const [lastUpdate, setLastUpdate] = useState<number>(0);
    const [now, setNow] = useState<number>(Date.now());
    const dropTimerRef = useRef<number | null>(null);
    const warnedRef = useRef<boolean>(false);
    const [listening, setListening] = useState<boolean>(false);

    /* Web Audio graph for the reconstructed tone. Kept in refs so re-renders do
     * not rebuild it and click on every update. */
    const audioContextRef = useRef<AudioContext | null>(null);
    const oscillatorRef = useRef<OscillatorNode | null>(null);
    const gainRef = useRef<GainNode | null>(null);

    /* Latest sensor values, read by the audio updater without re-subscribing. */
    const sensorsRef = useRef<KundtSensors>(sensors);
    sensorsRef.current = sensors;

    const amplitude = sensors.mic_amplitude ?? 0;
    const rms = sensors.mic_rms ?? 0;
    const peak = sensors.mic_peak ?? 0;
    const toneRatio = rms > 0 ? (amplitude / rms) * 100 : 0;
    const saturating = peak >= PEAK_SATURATION;
    const headroom = peak > 0 ? PEAK_FULL_SCALE / peak : 0;

    /*
     * Estos tres derivados recorren y ordenan hasta 600 muestras. Sin memoizar
     * se rehacian en CADA render, incluido el tic de un segundo del indicador de
     * frescura, y como chartData nacia como array nuevo cada vez, @nivo volvia a
     * dibujar la grafica entera aunque no hubiera llegado ni un dato.
     */
    const resonances = useMemo(() => findResonances(samples), [samples]);
    const measuredSpeed = speedFromResonances(resonances, frequency);
    const expectedSpacing = frequency > 0 ? (REFERENCE_SPEED_MS / frequency / 2) * 100 : 0;

    /* Reloj propio: sin el, "hace 4 s" se quedaria escrito hasta que llegara el
     * siguiente dato, que es justo el caso en que interesa que avance. */
    useEffect(() => {
        const tick = window.setInterval(() => setNow(Date.now()), 1000);
        return () => window.clearInterval(tick);
    }, []);

    useEffect(() => {
        getCameraList(platform_id)
            .then(setCameras)
            .catch(() => showSnackBar({ severity: "error", message: "No se pudieron cargar las cámaras." }));
    }, [platform_id]);

    useEffect(() => {
        const closeSSE = startAuthenticatedSSE<KundtSSE>({
            url: SSE_KUNDT_UPDATES,
            onOpen: () => {
                setLinkUp(true);
                /* Si veniamos de un corte sostenido, avisar de la recuperacion:
                 * el usuario ya vio el error y merece saber que se arreglo. */
                if (warnedRef.current) {
                    warnedRef.current = false;
                    showSnackBar({ severity: "success", message: "Conexión con el experimento restablecida." });
                }
                if (dropTimerRef.current) {
                    window.clearTimeout(dropTimerRef.current);
                    dropTimerRef.current = null;
                }
            },
            onMessage: (data) => {
                /*
                 * El flujo no esta filtrado por plataforma: la API emite la fila
                 * de cualquier tubo que reporte. Con un solo kit da igual, con
                 * cinco no.
                 */
                if (data.id !== initial.id) return;

                setLinkUp(true);
                setLastUpdate(Date.now());

                setSensors({
                    mic_amplitude: data.mic_amplitude,
                    mic_rms: data.mic_rms,
                    mic_peak: data.mic_peak,
                    plunger_actual: data.plunger_actual,
                    clockwise_limit: data.clockwise_limit,
                    counter_limit: data.counter_limit,
                });

                /*
                 * La fila llega entera 13 veces por segundo, asi que posicion y
                 * amplitud siempre vienen juntas. Se guarda un punto solo cuando
                 * alguna de las dos cambia: si no, la curva se llenaria de cientos
                 * de muestras identicas con el embolo quieto y el historial util
                 * quedaria reducido a unos segundos.
                 */
                setSamples((previous) => {
                    const last = previous[previous.length - 1];
                    if (last && last.position === data.plunger_actual && last.amplitude === data.mic_amplitude) {
                        return previous;
                    }
                    const next = [...previous, { position: data.plunger_actual, amplitude: data.mic_amplitude }];
                    return next.length > MAX_SAMPLES ? next.slice(next.length - MAX_SAMPLES) : next;
                });

                setFrequency(data.frequency);
                if (!draggingRef.current) {
                    setFreqDraft(data.frequency);
                    setFreqText(String(data.frequency));
                    setVolumeDraft(data.volume);
                    setPlungerDraft(data.plunger_pos);
                }
            },
            onError: (error) => {
                /*
                 * fetchEventSource reintenta solo, y llama a onError tambien
                 * cuando la conexion se aborta a proposito: al desmontar, y en
                 * desarrollo en cada montaje doble de StrictMode. Avisar en el
                 * acto producia el falso "se perdio la conexion" con el flujo
                 * perfectamente vivo. Solo se avisa si sigue caida un rato.
                 */
                if (error instanceof DOMException && error.name === "AbortError") return;
                setLinkUp(false);
                if (dropTimerRef.current === null) {
                    dropTimerRef.current = window.setTimeout(() => {
                        dropTimerRef.current = null;
                        warnedRef.current = true;
                        showSnackBar({ severity: "error", message: "Se perdió la conexión con el experimento." });
                    }, LINK_GRACE_MS);
                }
            },
        });
        return () => {
            if (dropTimerRef.current) window.clearTimeout(dropTimerRef.current);
            dropTimerRef.current = null;
            closeSSE();
        };
    }, []);

    /* Reconstructed audio. The microphone sends scalars, never waveforms, so
     * this is a synthesised tone at the driven frequency whose gain follows the
     * Goertzel magnitude. It shows the resonance; it is not what the mic heard. */
    useEffect(() => {
        if (!listening) return;

        /*
         * El contexto ya viene creado y reanudado desde el manejador del clic
         * (toggleListening). Crearlo aqui lo dejaria en estado "suspended": los
         * navegadores solo permiten arrancar audio dentro de la pila de llamadas
         * de un gesto del usuario, y el efecto de React corre despues. Sin eso
         * oscillator.start() no falla ni avisa, simplemente no suena.
         */
        const context = audioContextRef.current;
        if (!context) return;

        const oscillator = context.createOscillator();
        const gain = context.createGain();

        oscillator.type = "sine";
        oscillator.frequency.setValueAtTime(frequency, context.currentTime);
        gain.gain.setValueAtTime(0, context.currentTime);
        oscillator.connect(gain);
        gain.connect(context.destination);
        oscillator.start();

        oscillatorRef.current = oscillator;
        gainRef.current = gain;

        /*
         * La ganancia sigue la AMPLITUD contra el fondo de escala del ADC, no el
         * cociente amp/rms. El cociente mide que fraccion del nivel es el tono,
         * y se mantiene casi constante al atravesar una resonancia: usarlo aqui
         * hacia que el tubo sonara igual en un nodo que en un vientre, que es
         * exactamente lo que esta funcion existe para dejar oir.
         *
         * La raiz cuadrada comprime el recorrido: entre valle y pico se han
         * medido factores de x12, y en escala lineal el valle quedaria inaudible.
         * El 0,25 final evita que una resonancia resulte molesta.
         */
        const interval = window.setInterval(() => {
            const level = sensorsRef.current.mic_amplitude ?? 0;
            const normalised = Math.min(1, Math.sqrt(Math.max(level, 0) / PEAK_FULL_SCALE));
            gain.gain.linearRampToValueAtTime(normalised * 0.25, context.currentTime + 0.15);
        }, 120);

        return () => {
            window.clearInterval(interval);
            try { oscillator.stop(); } catch { /* already stopped */ }
            oscillator.disconnect();
            gain.disconnect();
            /* No se cierra el contexto: close() es irreversible y volver a
             * escuchar exigiria otro gesto del usuario. Se deja vivo y en
             * silencio para que el siguiente "Escuchar" sea inmediato. */
            oscillatorRef.current = null;
            gainRef.current = null;
        };
    }, [listening]);

    useEffect(() => {
        const context = audioContextRef.current;
        const oscillator = oscillatorRef.current;
        if (context && oscillator) {
            oscillator.frequency.linearRampToValueAtTime(frequency, context.currentTime + 0.05);
        }
    }, [frequency]);

    /*
     * Crear y reanudar el contexto DENTRO del clic. Es el unico momento en que
     * el navegador lo permite. Si aun asi no queda en "running", se avisa: un
     * audio bloqueado que no dice nada es indistinguible de uno averiado.
     */
    const toggleListening = async () => {
        if (listening) {
            setListening(false);
            return;
        }
        try {
            const context = audioContextRef.current ?? new AudioContext();
            audioContextRef.current = context;
            if (context.state === "suspended") await context.resume();
            if (context.state !== "running") {
                showSnackBar({
                    severity: "warning",
                    message: "El navegador bloqueó el audio. Permite el sonido para este sitio y vuelve a intentarlo.",
                });
                return;
            }
            setListening(true);
        }
        catch {
            showSnackBar({ severity: "error", message: "Este navegador no permite Web Audio." });
        }
    };

    const send = async (body: Parameters<typeof updateKundt>[1], message?: string) => {
        try {
            await updateKundt(platform_id, body);
            if (message) showSnackBar({ severity: "success", message });
        }
        catch {
            showSnackBar({ severity: "error", message: "No se pudo enviar la instrucción al experimento." });
        }
    };

    const commitFrequency = (hz: number) => {
        setFrequency(hz);
        setFreqDraft(hz);
        setFreqText(String(hz));
        send({ frequency: hz });
    };

    /*
     * Consigna escrita a mano. Se acota al rango del firmware en vez de
     * rechazarla: E2 ya acota igual (hallazgo A4), asi que rechazar aqui solo
     * añadiria un aviso que el hardware no respeta.
     */
    const commitFrequencyText = () => {
        const parsed = Number(freqText);
        if (!Number.isFinite(parsed) || freqText.trim() === "") {
            setFreqText(String(freqDraft));
            showSnackBar({ severity: "warning", message: "La frecuencia debe ser un número." });
            return;
        }
        const clamped = Math.round(Math.min(FREQ_MAX_HZ, Math.max(FREQ_MIN_HZ, parsed)));
        if (clamped !== parsed) {
            showSnackBar({
                severity: "info",
                message: `Fuera de rango: ajustada a ${clamped} Hz (${FREQ_MIN_HZ}-${FREQ_MAX_HZ} Hz).`,
            });
        }
        commitFrequency(clamped);
    };

    const commitVolume = (value: number) => {
        setVolumeDraft(value);
        send({ volume: value });
    };

    const commitPlunger = (cm: number) => {
        send({ plunger_pos: cm }, `Émbolo enviado a ${cm} cm.`);
    };

    const calibrate = () => {
        send({ calibrate: true }, "Calibración solicitada: el émbolo buscará el fin de carrera.");
    };

    /* Edad del ultimo dato. lastUpdate en 0 significa que aun no ha llegado
     * ninguno: lo que se ve es lo que trajo el cargador al abrir la pagina. */
    const dataAgeMs = lastUpdate === 0 ? Infinity : now - lastUpdate;
    const fresh = linkUp && dataAgeMs < STALE_MS;
    const ageLabel = lastUpdate === 0
        ? "sin datos en vivo"
        : dataAgeMs < 2000 ? "en vivo" : `hace ${Math.round(dataAgeMs / 1000)} s`;

    const chartData = useMemo(() => [{
        id: "Amplitud",
        data: [...samples]
            .sort((a, b) => a.position - b.position)
            .map((s) => ({ x: s.position, y: Math.round(s.amplitude) })),
    }], [samples]);

    const resonanceRows = useMemo(() => resonances.map((position, index) => ({
        id: index,
        orden: index + 1,
        posicion: position.toFixed(1),
        separacion: index === 0 ? "—" : (position - resonances[index - 1]).toFixed(1),
    })), [resonances]);

    return (
        <Stack spacing={2.5} sx={{ padding: 2 }}>

            {/* Estado del enlace. Sin esto, un flujo caido y uno vivo con el
                embolo quieto se ven exactamente igual. */}
            <Stack direction="row" spacing={1} alignItems="center" justifyContent="flex-end">
                <Chip
                    size="small"
                    color={fresh ? "success" : linkUp ? "warning" : "error"}
                    variant={fresh ? "filled" : "outlined"}
                    label={linkUp ? `Datos: ${ageLabel}` : "Sin conexión con el experimento"}
                />
            </Stack>

            <CamerasDisplay camera_list={cameras} flipped_cameras={FLIPPED_CAMERAS} />

            <Grid container spacing={2.5}>

                {/* ---------------- Generador de audio (E2) ---------------- */}
                <Grid size={{ xs: 12, md: 6 }}>
                    <Card sx={{ borderRadius: CONTROL_BORDER_CARD_RAIDUS, height: "100%" }}>
                        <CardContent>
                            <CardTitle label="Generador de audio" icon={<GraphicEqIcon />} />

                            <Stack spacing={2} sx={{ marginTop: 2 }}>
                                <Box>
                                    <Typography variant="body2" sx={{ marginBottom: 1 }}>
                                        Frecuencia
                                    </Typography>
                                    {/* Las etiquetas de marks van en posicion absoluta y sobresalen por
                                        debajo de la caja del Slider: sin este hueco caen encima del
                                        texto de ayuda que viene despues. */}
                                    <Stack direction="row" spacing={1.5} alignItems="center" sx={{ mb: 3 }}>
                                        <GraphicEqIcon fontSize="small" color="action" />
                                        <TextField
                                            value={freqText}
                                            onChange={(e) => setFreqText(e.target.value)}
                                            onKeyDown={(e) => { if (e.key === "Enter") commitFrequencyText(); }}
                                            onBlur={commitFrequencyText}
                                            size="small"
                                            type="number"
                                            sx={{ width: 130, flexShrink: 0 }}
                                            slotProps={{
                                                input: { endAdornment: <InputAdornment position="end">Hz</InputAdornment> },
                                                htmlInput: { min: FREQ_MIN_HZ, max: FREQ_MAX_HZ, step: 1, "aria-label": "Frecuencia en hercios" },
                                            }}
                                        />
                                        <Slider
                                            value={freqToSlider(freqDraft)}
                                            min={0}
                                            max={100}
                                            step={0.05}
                                            marks={FREQ_MARKS}
                                            valueLabelDisplay="auto"
                                            valueLabelFormat={() => `${freqDraft} Hz`}
                                            onChange={(_, v) => { draggingRef.current = true; const hz = sliderToFreq(v as number); setFreqDraft(hz); setFreqText(String(hz)); }}
                                            onChangeCommitted={(_, v) => { draggingRef.current = false; commitFrequency(sliderToFreq(v as number)); }}
                                        />
                                    </Stack>
                                    <Typography variant="caption" color="text.secondary">
                                        Escala logarítmica de {FREQ_MIN_HZ} Hz a {FREQ_MAX_HZ / 1000} kHz.
                                        La caja y la barra están sincronizadas; la caja aplica con Enter o al salir.
                                    </Typography>
                                </Box>

                                <Stack direction="row" spacing={1} flexWrap="wrap" useFlexGap>
                                    {[350, 500, 1000, 1500, 2000].map((preset) => (
                                        <Chip
                                            key={preset}
                                            label={`${preset} Hz`}
                                            onClick={() => commitFrequency(preset)}
                                            color={frequency === preset ? "primary" : "default"}
                                            variant={frequency === preset ? "filled" : "outlined"}
                                        />
                                    ))}
                                </Stack>

                                <Box>
                                    <Typography variant="body2" sx={{ marginBottom: 1 }}>
                                        Volumen: <b>{volumeDraft}°</b> de {SERVO_MAX_DEG}°
                                    </Typography>
                                    {/* Las etiquetas de marks van en posicion absoluta y sobresalen por
                                        debajo de la caja del Slider: sin este hueco caen encima del
                                        texto de ayuda que viene despues. */}
                                    <Stack direction="row" spacing={1.5} alignItems="center" sx={{ mb: 3 }}>
                                        <VolumeUpIcon fontSize="small" color="action" />
                                        <Slider
                                            value={volumeDraft}
                                            min={0}
                                            max={SERVO_MAX_DEG}
                                            step={1}
                                            marks={VOLUME_MARKS}
                                            valueLabelDisplay="auto"
                                            onChange={(_, v) => { draggingRef.current = true; setVolumeDraft(v as number); }}
                                            onChangeCommitted={(_, v) => { draggingRef.current = false; commitVolume(v as number); }}
                                        />
                                    </Stack>
                                    <Typography variant="caption" color="text.secondary">
                                        Es el ángulo del servo que gira el potenciómetro, no un porcentaje.
                                    </Typography>
                                </Box>
                            </Stack>
                        </CardContent>
                    </Card>
                </Grid>

                {/* ---------------- Émbolo (E3) ---------------- */}
                <Grid size={{ xs: 12, md: 6 }}>
                    <Card sx={{ borderRadius: CONTROL_BORDER_CARD_RAIDUS, height: "100%" }}>
                        <CardContent>
                            <CardTitle label="Émbolo" icon={<StraightenIcon />} />

                            <Stack spacing={2} sx={{ marginTop: 2 }}>
                                <Typography variant="h5">
                                    {sensors.plunger_actual !== undefined ? `${sensors.plunger_actual.toFixed(2)} cm` : "sin dato"}
                                </Typography>

                                <Box>
                                    <Typography variant="body2" sx={{ marginBottom: 1 }}>
                                        Ir a: <b>{plungerDraft.toFixed(1)} cm</b>
                                    </Typography>
                                    {/* Las etiquetas de marks van en posicion absoluta y sobresalen por
                                        debajo de la caja del Slider: sin este hueco caen encima del
                                        texto de ayuda que viene despues. */}
                                    <Stack direction="row" spacing={1.5} alignItems="center" sx={{ mb: 3 }}>
                                        <StraightenIcon fontSize="small" color="action" />
                                        <Slider
                                            value={toSliderCm(plungerDraft)}
                                            min={toSliderCm(PLUNGER_MAX_CM)}
                                            max={toSliderCm(PLUNGER_MIN_CM)}
                                            step={0.5}
                                            marks={PLUNGER_MARKS}
                                            valueLabelDisplay="auto"
                                            valueLabelFormat={(v) => `${fromSliderCm(v)} cm`}
                                            onChange={(_, v) => { draggingRef.current = true; setPlungerDraft(fromSliderCm(v as number)); }}
                                            onChangeCommitted={(_, v) => { draggingRef.current = false; commitPlunger(fromSliderCm(v as number)); }}
                                        />
                                    </Stack>
                                    <Typography variant="caption" color="text.secondary">
                                        Recorrido útil del riel: {PLUNGER_MIN_CM} a {PLUNGER_MAX_CM} cm desde el parlante.
                                        La barra va invertida a propósito: se mueve en el mismo sentido que el émbolo.
                                    </Typography>
                                </Box>

                                <Stack direction="row" spacing={1}>
                                    <Chip
                                        label={sensors.clockwise_limit ? "Fin de carrera +" : "Carrera + libre"}
                                        color={sensors.clockwise_limit ? "warning" : "default"}
                                        size="small"
                                    />
                                    <Chip
                                        label={sensors.counter_limit ? "Fin de carrera −" : "Carrera − libre"}
                                        color={sensors.counter_limit ? "warning" : "default"}
                                        size="small"
                                    />
                                </Stack>

                                <ButtonCB variant="outlined" startIcon={<HomeIcon />} onClick={calibrate}>
                                    Calibrar
                                </ButtonCB>
                                <Typography variant="caption" color="text.secondary">
                                    Lleva el émbolo contra el fin de carrera y fija el cero. Hazlo antes de medir.
                                </Typography>
                            </Stack>
                        </CardContent>
                    </Card>
                </Grid>

                {/* ---------------- Micrófono (E1) ---------------- */}
                <Grid size={{ xs: 12, md: 6 }}>
                    <Card sx={{ borderRadius: CONTROL_BORDER_CARD_RAIDUS, height: "100%" }}>
                        <CardContent>
                            <CardTitle label="Micrófono" icon={<GraphicEqIcon />} />

                            <Stack spacing={2} sx={{ marginTop: 2 }}>
                                <Typography variant="h4">{Math.round(amplitude)}</Typography>
                                <Typography variant="caption" color="text.secondary">
                                    Amplitud a {frequency} Hz, medida por Goertzel
                                </Typography>

                                <Box>
                                    <Typography variant="body2">
                                        Señal útil: <b>{toneRatio.toFixed(1)} %</b>
                                    </Typography>
                                    <LinearProgress
                                        variant="determinate"
                                        value={Math.min(100, toneRatio)}
                                        color={toneRatio >= TONE_RATIO_THRESHOLD ? "success" : "warning"}
                                    />
                                    <Typography variant="caption" color="text.secondary">
                                        Cuánto del nivel es el tono y no ruido. Por encima del {TONE_RATIO_THRESHOLD} % la medida es fiable.
                                    </Typography>
                                </Box>

                                <Box>
                                    <Typography variant="body2">
                                        Nivel de entrada: <b>{((peak / PEAK_FULL_SCALE) * 100).toFixed(0)} %</b>
                                        {headroom > 0 && !saturating ? ` · margen ×${headroom.toFixed(1)}` : ""}
                                    </Typography>
                                    <LinearProgress
                                        variant="determinate"
                                        value={Math.min(100, (peak / PEAK_FULL_SCALE) * 100)}
                                        color={saturating ? "error" : "primary"}
                                    />
                                </Box>

                                {saturating && (
                                    <Alert severity="error">
                                        El micrófono está saturando: la amplitud deja de ser fiable.
                                        Baja el volumen o la ganancia del preamplificador.
                                    </Alert>
                                )}
                                {!saturating && headroom > 0 && headroom < 4 && (
                                    <Alert severity="warning">
                                        Poco margen (×{headroom.toFixed(1)}). Al entrar en resonancia la amplitud
                                        se multiplica y puede recortar. Ajusta fuera de resonancia dejando ×4.
                                    </Alert>
                                )}
                            </Stack>
                        </CardContent>
                    </Card>
                </Grid>

                {/* ---------------- Audio reconstruido ---------------- */}
                <Grid size={{ xs: 12, md: 6 }}>
                    <Card sx={{ borderRadius: CONTROL_BORDER_CARD_RAIDUS, height: "100%" }}>
                        <CardContent>
                            <CardTitle label="Escuchar" icon={<VolumeUpIcon />} />

                            <Stack spacing={2} sx={{ marginTop: 2 }}>
                                <ButtonCB
                                    variant="contained"
                                    color={listening ? "error" : "primary"}
                                    startIcon={listening ? <StopIcon /> : <PlayArrowIcon />}
                                    onClick={toggleListening}
                                >
                                    {listening ? "Detener" : "Escuchar el tubo"}
                                </ButtonCB>

                                <Alert severity="info">
                                    Es una <b>reconstrucción</b>. El micrófono envía la amplitud medida, no la onda:
                                    el navegador sintetiza un tono a {frequency} Hz y sigue esa amplitud.
                                    Oirás cómo sube y baja al pasar por las resonancias, pero no el timbre real del tubo.
                                </Alert>
                            </Stack>
                        </CardContent>
                    </Card>
                </Grid>

                {/* ---------------- Curva de resonancia ---------------- */}
                <Grid size={{ xs: 12 }}>
                    <Card sx={{ borderRadius: CONTROL_BORDER_CARD_RAIDUS }}>
                        <CardContent>
                            <Stack direction="row" spacing={2} alignItems="center" justifyContent="space-between" flexWrap="wrap" useFlexGap>
                                <CardTitle label="Curva de resonancia" icon={<GraphicEqIcon />} />
                                <ButtonCB variant="outlined" startIcon={<DeleteSweepIcon />} onClick={() => setSamples([])}>
                                    Limpiar
                                </ButtonCB>
                            </Stack>

                            <Box sx={{ height: 360, marginTop: 2 }}>
                                {samples.length === 0 ? (
                                    <Typography variant="body2" color="text.secondary" sx={{ padding: 4 }}>
                                        Aún no hay medidas. Mueve el émbolo y la curva se irá construyendo:
                                        cada punto es una amplitud medida en una posición.
                                    </Typography>
                                ) : (
                                    <ResponsiveLine
                                        data={chartData}
                                        margin={{ top: 30, right: 40, bottom: 70, left: 70 }}
                                        axisBottom={{ legend: "Posición del émbolo [cm]", legendOffset: 40, legendPosition: "middle" }}
                                        axisLeft={{ legend: "Amplitud", legendOffset: -55, legendPosition: "middle" }}
                                        pointSize={4}
                                        useMesh={true}
                                        theme={generatePlotTheme(theme)}
                                        colors={{ scheme: "category10" }}
                                        xScale={{ type: "linear", min: "auto", max: "auto" }}
                                        yScale={{ type: "linear", min: 0, max: "auto" }}
                                    />
                                )}
                            </Box>
                        </CardContent>
                    </Card>
                </Grid>

                {/* ---------------- Velocidad del sonido ---------------- */}
                <Grid size={{ xs: 12 }}>
                    <Card sx={{ borderRadius: CONTROL_BORDER_CARD_RAIDUS }}>
                        <CardContent>
                            <CardTitle label="Velocidad del sonido" icon={<StraightenIcon />} />

                            <Grid container spacing={2} sx={{ marginTop: 1 }}>
                                <Grid size={{ xs: 12, md: 5 }}>
                                    <Stack spacing={1}>
                                        <Typography variant="h4">
                                            {measuredSpeed !== null ? `${measuredSpeed.toFixed(1)} m/s` : "—"}
                                        </Typography>
                                        <Typography variant="caption" color="text.secondary">
                                            v = 2 · separación entre resonancias · frecuencia
                                        </Typography>
                                        <Typography variant="body2">
                                            Resonancias detectadas: <b>{resonances.length}</b>
                                        </Typography>
                                        <Typography variant="body2" color="text.secondary">
                                            A {frequency} Hz deberían estar cada {expectedSpacing.toFixed(1)} cm
                                            si la velocidad fuera {REFERENCE_SPEED_MS} m/s.
                                        </Typography>
                                        {resonances.length < 2 && (
                                            <Alert severity="info">
                                                Hacen falta al menos dos resonancias. Barre el émbolo por todo
                                                el recorrido sin saltarte tramos.
                                            </Alert>
                                        )}
                                    </Stack>
                                </Grid>

                                <Grid size={{ xs: 12, md: 7 }}>
                                    {resonanceRows.length > 0 && (
                                        <UserGrid
                                            data_rows={resonanceRows}
                                            header_cols={[
                                                { field: "orden", headerName: "Máximo", flex: 1 },
                                                { field: "posicion", headerName: "Posición [cm]", flex: 1 },
                                                { field: "separacion", headerName: "Separación [cm]", flex: 1 },
                                            ]}
                                            disable_check
                                        />
                                    )}
                                </Grid>
                            </Grid>
                        </CardContent>
                    </Card>
                </Grid>

            </Grid>
        </Stack>
    );
}
