package com.anatdx.kagami;

interface IKasumiRootService {
    String snapshotJson();
    String setEnabled(boolean enabled);
    String setDebug(boolean enabled);
    String setMountHide(boolean enabled);
    String setMapsSpoof(boolean enabled);
    String setStatfsSpoof(boolean enabled);
    String setSelinuxGuard(boolean enabled);
    String hidePath(String path);
    String deleteRule(String path);
    String addMapsRule(String targetIno, String targetDev, String spoofedIno, String spoofedDev, String spoofedPath);
    String clearMapsRules();
    String setPolicy(String owner, int flags);
    String setPolicyUids(String list, in int[] uids);
    String clearPolicyUids(String list);
    String kernelLog();
    String statPath(String path);
}
