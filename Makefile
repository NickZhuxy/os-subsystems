.PHONY: dev test image bench sched-bench vm-bench vm-lackey fs-evidence sync-bench repro
DEV_IMAGE := os-subsystems-dev:local
RUNTIME_IMAGE := os-subsystems:local
dev:
	docker build --target dev -f docker/Dockerfile -t $(DEV_IMAGE) .
test: dev
	docker run --rm --cpus=4 --memory=4g --security-opt seccomp=unconfined --security-opt apparmor=unconfined --device /dev/fuse --cap-add SYS_ADMIN -e OS_FS_TEST_FUSE=1 --ulimit nofile=65536:65536 -v "$(CURDIR):/checkout:ro" $(DEV_IMAGE) sh /checkout/scripts/container.sh sh scripts/test.sh
image:
	docker build --target runtime -f docker/Dockerfile -t $(RUNTIME_IMAGE) .
bench: dev
	mkdir -p results
	docker run --rm --cpus=4 --memory=4g --ulimit nofile=65536:65536 -e HOST_LABEL -v "$(CURDIR):/checkout:ro" -v "$(CURDIR)/results:/src/results" $(DEV_IMAGE) sh /checkout/scripts/container.sh python3 scripts/bench.py
sched-bench: dev
	mkdir -p results
	docker run --rm --cpus=4 --memory=4g -v "$(CURDIR):/checkout:ro" -v "$(CURDIR)/results:/src/results" $(DEV_IMAGE) sh /checkout/scripts/container.sh python3 scripts/sched_bench.py
repro: dev
	mkdir -p results/m7-final
	docker run --rm --cpus=4 --memory=4g -v "$(CURDIR):/checkout:ro" -v "$(CURDIR)/results:/src/results" $(DEV_IMAGE) sh /checkout/scripts/container.sh sh scripts/repro.sh
vm-bench: dev
	mkdir -p results
	docker run --rm --cpus=4 --memory=4g -v "$(CURDIR):/checkout:ro" -v "$(CURDIR)/results:/src/results" $(DEV_IMAGE) sh /checkout/scripts/container.sh python3 scripts/vm_bench.py
vm-lackey: dev
	mkdir -p results
	docker run --rm --cpus=4 --memory=4g -v "$(CURDIR):/checkout:ro" -v "$(CURDIR)/results:/src/results" $(DEV_IMAGE) sh /checkout/scripts/container.sh python3 scripts/vm_lackey.py
fs-evidence: dev
	mkdir -p results
	docker run --rm --cpus=4 --memory=4g --security-opt apparmor=unconfined --device /dev/fuse --cap-add SYS_ADMIN -e OS_FS_TEST_FUSE=1 -v "$(CURDIR):/checkout:ro" -v "$(CURDIR)/results:/src/results" $(DEV_IMAGE) sh /checkout/scripts/container.sh python3 scripts/fs_evidence.py
sync-bench: dev
	mkdir -p results
	docker run --rm --cpus=4 --memory=4g -v "$(CURDIR):/checkout:ro" -v "$(CURDIR)/results:/src/results" $(DEV_IMAGE) sh /checkout/scripts/container.sh python3 scripts/sync_bench.py
