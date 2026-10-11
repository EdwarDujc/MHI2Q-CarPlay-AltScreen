package com.luka.carplay.routeguidance;

import java.lang.reflect.Method;
import com.luka.carplay.framework.Log;

/* Preserve our existing OEM gray-bar fields alongside the new RGI renderer.
 * Only owned fields are cleared, before the native BAP gate is released. */
final class LowerBarKomo {
    private boolean roadOwned, distanceOwned, arrivalOwned;
    private RouteGuidance.State cached;

    void update(Object cluster, RouteGuidance.State s) {
        if (cluster == null) return;
        int dirty = s.dirtyMask;
        if (cached == null) cached = new RouteGuidance.State();
        if ((dirty & RouteGuidance.State.DIRTY_CURRENT_ROAD) != 0) cached.currentRoad = s.currentRoad;
        if ((dirty & RouteGuidance.State.DIRTY_DIST_DEST) != 0) cached.distDestM = s.distDestM;
        if ((dirty & RouteGuidance.State.DIRTY_ETA) != 0) cached.etaSeconds = s.etaSeconds;
        cached.dirtyMask |= dirty & (RouteGuidance.State.DIRTY_CURRENT_ROAD
            | RouteGuidance.State.DIRTY_DIST_DEST | RouteGuidance.State.DIRTY_ETA);
        if ((dirty & RouteGuidance.State.DIRTY_CURRENT_ROAD) != 0) {
            boolean valid = s.currentRoad != null && s.currentRoad.trim().length() > 0;
            if (valid || roadOwned) {
                call(cluster, "updateCurrentStreet", new Class[]{String.class},
                    new Object[]{valid ? s.currentRoad : ""});
                roadOwned = valid;
            }
        }
        if ((dirty & RouteGuidance.State.DIRTY_DIST_DEST) != 0) {
            boolean valid = s.distDestM >= 0;
            if (valid || distanceOwned) {
                call(cluster, "updateDistanceToDestination", new Class[]{Integer.TYPE, Boolean.TYPE},
                    new Object[]{new Integer(valid ? s.distDestM : 0), Boolean.FALSE});
                flush(cluster);
                distanceOwned = valid;
            }
        }
        if ((dirty & RouteGuidance.State.DIRTY_ETA) != 0) {
            boolean valid = s.etaSeconds >= 0;
            if (valid || arrivalOwned) {
                call(cluster, "updateArrivalTime", new Class[]{Boolean.TYPE, Long.TYPE, Boolean.TYPE},
                    new Object[]{valid ? Boolean.TRUE : Boolean.FALSE,
                        new Long(valid ? s.etaSeconds * 1000L : 0L), Boolean.FALSE});
                flush(cluster);
                arrivalOwned = valid;
            }
        }
    }

    void clear(Object cluster) {
        RouteGuidance.State empty = new RouteGuidance.State();
        empty.dirtyMask = RouteGuidance.State.DIRTY_CURRENT_ROAD
            | RouteGuidance.State.DIRTY_DIST_DEST | RouteGuidance.State.DIRTY_ETA;
        update(cluster, empty);
        cached = null;
    }

    void clearForRebind(Object cluster) {
        RouteGuidance.State snapshot = cached;
        cached = null;
        clear(cluster);
        cached = snapshot;
    }

    void replay(Object cluster) { if (cached != null) update(cluster, cached); }

    private static void flush(Object cluster) {
        call(cluster, "updateKOMOFollowInfo", new Class[0], new Object[0]);
    }

    private static void call(Object target, String name, Class[] signature, Object[] args) {
        try {
            Class type = target.getClass();
            Method method = null;
            while (type != null) {
                try { method = type.getDeclaredMethod(name, signature); break; }
                catch (NoSuchMethodException e) { type = type.getSuperclass(); }
            }
            if (method == null) throw new NoSuchMethodException(name);
            method.setAccessible(true);
            method.invoke(target, args);
        } catch (Throwable t) {
            Log.w("LowerBarKomo", name + " failed: " + t);
        }
    }
}
