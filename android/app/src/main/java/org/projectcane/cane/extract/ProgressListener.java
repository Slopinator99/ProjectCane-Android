package org.projectcane.cane.extract;

public interface ProgressListener {
    /** @param fraction overall progress 0..1 */
    void onProgress(String item, double fraction);
    boolean isCancelled();
}
