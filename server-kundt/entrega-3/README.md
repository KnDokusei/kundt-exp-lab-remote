# Entrega 3: ajustes a la vista de Kundt

Cinco parches sobre `curiousBeagleFront` `64e32e2`, el merge del #77 que trajo la
vista. Se abrieron como el pull request #78, que se fusionó el 2026-10-02 y ya
está en producción.

| Parche | Qué cambia |
|---|---|
| `0001` | La caja de frecuencia ya no se sobrescribe con cada evento SSE mientras se escribe en ella |
| `0002` | `CameraViewer` y `CamerasDisplay` aceptan una relación de aspecto opcional (16:9 por omisión). Kundt pide 4:3, que es lo que entregan sus cámaras, y desaparecen las franjas negras |
| `0003` | Fuera el volteo de cámara en la web: lo hace el sensor de la cámara montada al revés |
| `0004` | Textos de ayuda más cortos y sin el botón Calibrar, que E3 todavía no atiende |
| `0005` | Tabla de mediciones (d_min, f, λ y λ/2, en metros) y ajuste lineal de f frente a 1/λ. La pendiente es la velocidad del sonido. La curva de resonancia no se toca |

Sólo `0002` toca componentes compartidos con otros experimentos, y sin cambiar
su comportamiento: si no se pasa la relación de aspecto, queda en 16:9 como antes.

## Aplicar

```bash
cd curiousBeagleFront
git am /ruta/a/entrega-3/front/*.patch
```

Probado en el banco local el 2026-10-01, con una placa simulada y las tres
cámaras reales del kit 1 a través de go2rtc.

## Desplegar

No hay CI. En el servidor, desde la raíz de `curiousBeagle`:

```bash
git -C curiousBeagleFront pull
docker compose up -d --build front
```
