try {
    const maplibregl = await import('./maplibre-gl.mjs');
    globalThis.maplibregl = maplibregl;
    maplibregl.setWorkerUrl('/maplibre-gl-worker.mjs');
    maplibregl.setWorkerCount(1);
    maplibregl.setMaxParallelImageRequests(2);

    await new Promise((resolve, reject) => {
        const plugin = document.createElement('script');
        plugin.src = '/leaflet-maplibre-gl.js';
        plugin.onload = resolve;
        plugin.onerror = () => reject(new Error('MapLibre Leaflet bridge failed to load'));
        document.head.appendChild(plugin);
    });

    if (globalThis.pmtiles) {
        const protocol = new globalThis.pmtiles.Protocol();
        maplibregl.addProtocol('pmtiles', protocol.tile);
        globalThis.bluepawsPmtilesProtocol = protocol;
    }
} catch (error) {
    console.warn('Offline vector renderer unavailable; using raster fallback.', error);
}

await import('./app.js?v=20260911-pmtiles1');
