/***************************************************************************
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *    https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 * Author: Harihara Kadayam <harihara.kadayam@gmail.com>
 ***************************************************************************/

use sisl::reactor_local::ReactorLocal;

#[test]
fn test_reactor_local_compilation() {
    // This test verifies that ReactorLocal can be created and has the expected API.
    // It doesn't actually run on reactors since that requires IOManager
    // initialization in a more complex test setup.

    // Note: This would panic if IOManager is not initialized, but we're just
    // testing compilation and API surface here.
    //
    // In real usage:
    // 1. Initialize IOManager: iomgr::init_iomgr(num_cpus).unwrap();
    // 2. Create ReactorLocal: let counter = ReactorLocal::new(|| 0u64);
    // 3. Access from reactor: *counter.get() += 1;
    // 4. Collect values: counter.collect().await;

    // Verify types compile
    fn _compile_check() {
        let _counter: ReactorLocal<u64> = ReactorLocal::new(|| 0);
        let _vec: ReactorLocal<Vec<String>> = ReactorLocal::new(Vec::new);
        let _option: ReactorLocal<Option<i32>> = ReactorLocal::new(|| None);
    }
}
