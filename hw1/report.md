# 虛擬機作業一報告

組員名單
- 蔡存哲 b01902138
- 許軒 b01902054

### 影子堆棧

資料結構：
雜湊表（shadow_pair\*\*）(shadow_hash_list)以(shadow_pair)為單位，記錄用戶位址和宿主位址配對。堆疊(shadow_pair\*\*)(shack)以(shadow_pair\*)為元素，每個元素指向(shadow_hash_list)裡的某個(shadow_pair)。

push_shack(next_eip)：
以next_eip為鍵取得所在的(shadow_pair)的位址，如果尚未存在於雜湊表就插入(用戶位址, 宿主位址) = （next_eip, NULL）。用TCG產生的代碼：將此（shadow_pair\*）推到shack上。

shack_set_shadow(guest_eip, host_eip)：
以guest_eip為鍵取得所在的(shadow_pair)的位址，將宿主位址欄設為host_eip。如果尚未存在於雜湊表就插入(用戶位址, 宿主位址) = （guest_eip, host_eip）。

pop_shack(next_eip)：
用TCG產生的代碼：從shack彈出一個(shadow_pair\*)pair。若pair->用戶位址==next_eip且pair->宿主位址!=NULL則跳轉到宿主位址。


### 間接跳轉

在helper_lookup_ibtc中產生搜尋快取的程式，若成功則回傳搜尋結果，失敗就設立一個旗標，讓後續執行到的update_ibtc_entry來進行更新。


### 效能測量

用一個計算費波那契數的程式當作模擬對象來進行測量
```c 
#include <stdio.h>

int fib(int n) {
    if (n == 0 || n == 1) {
		return 1;
	} else {
		return fib(n - 1) + fib(n - 2);
	}
}

int main(int argc, const char *argv[])
{
	printf("%d\n", fib(38));
	return 0;
}
```
時間從6.2秒左右降到2.8秒左右

也測試了幾個mibench上的測試程式，都有顯著的效能提升