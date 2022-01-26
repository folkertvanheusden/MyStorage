#include <atomic>
#include <signal.h>
#include <stdio.h>
#include <vector>
#include <yaml-cpp/yaml.h>

#include "aoe.h"
#include "compresser.h"
#include "compresser_zlib.h"
#include "compresser_lzo.h"
#include "hash_sha384.h"
#include "logging.h"
#include "mirror.h"
#include "mirror_storage_backend.h"
#include "nbd.h"
#include "socket_listener_ipv4.h"
#include "storage_backend_aoe.h"
#include "storage_backend_compressed_dir.h"
#include "storage_backend_dedup.h"
#include "storage_backend_file.h"

std::atomic_bool stop_flag { false };

void sigh(int sig)
{
	stop_flag = true;
}

int main(int argc, char *argv[])
{
	setlog("mystorage.log", ll_info, ll_info);
	dolog(ll_info, "MyStorage starting");

	signal(SIGTERM, sigh);
	signal(SIGINT, sigh);

//	constexpr uint8_t aoe_client_mac[] = { 0x32, 0x00, 0x11, 0x22, 0x33, 0x44 };
//	storage_backend_aoe *sb_aoe = new storage_backend_aoe("aoe", { }, "c_aoe", aoe_client_mac, 66, 6, 0);

	socket_listener *sl = new socket_listener_ipv4("0.0.0.0", 10809);

	storage_backend *m1 = new storage_backend_file("mirror1", "/home/folkert/temp/mirror-file-mystorage.dat", { });
	mirror_storage_backend *sm1 = new mirror_storage_backend("mirror1-sb", m1);

	std::vector<mirror *> mirrors1;
	mirrors1.push_back(sm1);
	storage_backend *sb1 = new storage_backend_file("file1", "/home/folkert/temp/mystorage.dat", mirrors1);

	storage_backend *m2 = new storage_backend_file("mirror2", "/home/folkert/temp/mirror-dir-mystorage.dat", { });
	mirror_storage_backend *sm2 = new mirror_storage_backend("mirror2-sb", m2);

	compresser *c = new compresser_zlib(3);
	std::vector<mirror *> mirrors2;
	mirrors2.push_back(sm2);
	storage_backend *sb2 = new storage_backend_compressed_dir("dir1", "/home/folkert/temp/dir", 131072, 17179869184, c, mirrors2);

	storage_backend *sb3 = new storage_backend_compressed_dir("dir2", "/home/folkert/temp/ramdisk", 131072, 4294967296, c, { });

	storage_backend *sb4 = new storage_backend_file("file2", "/home/folkert/temp/ramdisk/test.dat", { });

//	std::vector<mirror *> mirrors3;
//	mirror_storage_backend *sm3 = new mirror_storage_backend("mirror3-sb", sb_aoe);
//	mirrors3.push_back(sm3);
//	storage_backend *sb5 = new storage_backend_file("file3", "/home/folkert/temp/mystorage16M.dat", mirrors3);

//	constexpr uint8_t aoe_client_mac2[] = { 0x32, 0x00, 0x11, 0x22, 0x33, 0x55 };
//	storage_backend_aoe *sb6 = new storage_backend_aoe("aoe2", { }, "c_aoe2", aoe_client_mac2, 66, 6, 0);

	storage_backend_dedup sbd("sbd", "/home/folkert/temp/dedup.kch", new hash_sha384(), { }, 4ll * 1024ll * 1024ll * 1024ll, 65536);

	std::vector<storage_backend *> storage_backends { sb1, sb2, sb3, sb4, /*sb5, sb6,*/ &sbd };

	nbd *nbd_ = new nbd(sl, storage_backends);

	constexpr uint8_t my_mac[] = { 0x32, 0x11, 0x22, 0x33, 0x44, 0x55 };
	aoe *aoe_ = new aoe("ata", sb4, my_mac, 0, 11, 1);

	for(;!stop_flag;)
		pause();

	std::vector<YAML::Node> servers;
	servers.push_back(nbd_->emit_configuration());
	servers.push_back(aoe_->emit_configuration());

	YAML::Node out;
	out["type"] = "MyStorage";
	out["cfg"] = servers;

	YAML::Emitter output;
	output << out;

	FILE *fh = fopen("mystorage.yaml", "w");
	if (fh) {
		fprintf(fh, "%s", output.c_str());
		fclose(fh);
	}

	dolog(ll_info, "MyStorage terminating");

//	delete sb_aoe;
	delete aoe_;
	delete nbd_;
	delete sb2;
	delete c;
	delete sb1;
	delete sl;

	fflush(nullptr);
	sync();

	dolog(ll_info, "MyStorage stopped");

	return 0;
}
