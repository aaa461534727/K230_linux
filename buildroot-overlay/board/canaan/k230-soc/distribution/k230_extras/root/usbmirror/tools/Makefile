# 主机侧自测 (不需要真手机): 用 socketpair 假冒 adbd 跑完整 adb 流程
test:
	gcc -O2 -Wall -o /tmp/mock_adbd_test mock_adbd_test.c ../adb.c ../rsa2048.c -I.. -lpthread -lusb-1.0
	/tmp/mock_adbd_test
	/tmp/mock_adbd_test --noauth

# 重新生成 adb 密钥 (公钥指纹会变, 手机里要删掉旧的再允许一次)
key:
	python3 gen_adb_key.py ../adb_key.h

.PHONY: test key

