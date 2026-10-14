/*
 * Native suspension test on disposable GEFS mounts.
 * modload -s path=/mnt/gefs/file [-s path2=/mnt/fork/file]
 *     [-i seconds=5] [-b inject=true] ./suspendtest.kmod
 * modunload suspendtest
 *
 * Hold a durable suspension across timer deadlines, check that no device I/O
 * occurs, and always resume before returning. The inject case checks rollback
 * after a recorded page-write error. Concurrent user I/O may be run separately
 * to verify that it waits for resume.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/module.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/namei.h>
#include <sys/fstrans.h>
#include <sys/mutex.h>
#include <sys/condvar.h>
#include <sys/lwp.h>
#include <sys/kernel.h>
#include <prop/proplib.h>
#include <fs/gefs/gefs.h>

MODULE(MODULE_CLASS_MISC, suspendtest, "gefs");

static int
run_test(prop_dictionary_t props)
{
	const char *path[2] = { NULL, NULL };
	struct vnode *vp[2] = { NULL, NULL };
	struct mount *mp[2] = { NULL, NULL };
	struct gefsmount *gm[2];
	long long r[2], w[2], f[2], r1, w1, f1;
	unsigned seconds = 5;
	bool inject = false;
	int n = 1, held = 0, frozen = 0, error = 0, olderror = 0, i;

	if(props == NULL || !prop_dictionary_get_string(props, "path", &path[0]))
		return EINVAL;
	if(prop_dictionary_get_string(props, "path2", &path[1]))
		n = 2;
	(void)prop_dictionary_get_uint(props, "seconds", &seconds);
	(void)prop_dictionary_get_bool(props, "inject", &inject);
	if(seconds == 0 || seconds > 60)
		return EINVAL;
	for(i = 0; i < n; i++){
		error = namei_simple_kernel(path[i], NSM_FOLLOW_NOEMULROOT, &vp[i]);
		if(error != 0)
			goto out;
		mp[i] = vp[i]->v_mount;
		if(strcmp(mp[i]->mnt_op->vfs_name, MOUNT_GEFS) != 0 ||
		    (i > 0 && mp[i] == mp[0])){
			error = EINVAL;
			goto out;
		}
		error = vfs_busy(mp[i]);
		if(error != 0)
			goto out;
		held++;
		gm[i] = VFSTOGEFS(mp[i]);
	}
	if(inject){
		if(vp[0]->v_type != VREG){
			error = EINVAL;
			goto out;
		}
		olderror = VTOGN(vp[0])->gn_werror;
		VTOGN(vp[0])->gn_werror = EIO;
	}
	error = vfs_suspend(mp[0], 1);
	if(inject){
		VTOGN(vp[0])->gn_werror = olderror;
		if(error == 0){
			vfs_resume(mp[0]);
			error = EINVAL;
		}else if(error == EIO && fstrans_getstate(mp[0]) == FSTRANS_NORMAL &&
		    !gm[0]->gm_suspended)
			error = VFS_SYNC(mp[0], MNT_WAIT, curlwp->l_cred);
		else
			error = EINVAL;
		printf("suspendtest: write-error rollback %s\n", error ? "FAILED" : "ok");
		goto out;
	}
	if(error != 0)
		goto out;
	frozen = 1;
	/* vfs_suspend holds the one global session lock until vfs_resume. */
	if(n == 2){
		error = VFS_SUSPENDCTL(mp[1], SUSPEND_SUSPEND);
		if(error != 0)
			goto out;
		frozen = 2;
	}
	for(i = 0; i < n; i++){
		error = VFS_SYNC(mp[i], MNT_WAIT, curlwp->l_cred);
		if(error != 0)
			goto out;
		if(!gm[i]->gm_suspended || gm[i]->gm_nread != 0){
			error = EIO;
			goto out;
		}
		gefsdevstats(gm[i]->gm_gd->gd_fd, &r[i], &w[i], &f[i]);
	}
	printf("suspendtest: holding %d mount(s) for %u seconds\n", n, seconds);
	(void)kpause("susptest", false, seconds * hz, NULL);
	for(i = 0; i < n; i++){
		gefsdevstats(gm[i]->gm_gd->gd_fd, &r1, &w1, &f1);
		if(r1 != r[i] || w1 != w[i] || f1 != f[i])
			error = EIO;
		printf("suspendtest: mount %d I/O while suspended: reads %lld writes %lld flushes %lld\n",
		    i, r1-r[i], w1-w[i], f1-f[i]);
	}
out:
	if(frozen == 2)
		(void)VFS_SUSPENDCTL(mp[1], SUSPEND_RESUME);
	if(frozen >= 1)
		vfs_resume(mp[0]);
	for(i = 0; i < held; i++)
		vfs_unbusy(mp[i]);
	for(i = 0; i < n; i++)
		if(vp[i] != NULL)
			vrele(vp[i]);
	return error;
}

static int
suspendtest_modcmd(modcmd_t cmd, void *arg)
{
	switch(cmd){
	case MODULE_CMD_INIT:
		return run_test(arg);
	case MODULE_CMD_FINI:
		return 0;
	default:
		return ENOTTY;
	}
}
