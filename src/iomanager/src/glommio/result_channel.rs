//! Glommio-specific result channel implementation.

use crate::result_channel::{MessageId, ResultSender};
use glommio::channels::shared_channel;

/// Message type sent through the channel: (message_id, boxed_result)
pub type ChannelMessage = (MessageId, Box<dyn std::any::Any + Send + 'static>);

/// Glommio-specific result channel type.
/// Owns both sender and receiver - no split ownership!
pub type ResultChannel = crate::result_channel::ResultChannel<
    shared_channel::SharedSender<ChannelMessage>,
    shared_channel::SharedReceiver<ChannelMessage>,
>;

/// Create a new result channel with a glommio shared channel.
/// Returns a single ResultChannel that owns both sender and receiver.
pub fn create() -> ResultChannel {
    let (tx, rx) = shared_channel::new_bounded(1024); // Bounded channel with reasonable capacity
    crate::result_channel::ResultChannel::new(tx, rx)
}

impl ResultChannel {
    /// Run the receiver loop - processes incoming cross-reactor results.
    /// This takes the receiver out and runs until the channel closes.
    /// Can only be called once (receiver is taken).
    pub async fn run_receiver_loop(&self) {
        let mut receiver = self.take_receiver()
            .expect("run_receiver_loop called twice");
        
        while let Ok((msg_id, result)) = receiver.recv().await {
            self.put_result(msg_id, result);
        }
    }
}

/// Glommio-specific ResultSender implementation
impl ResultSender<shared_channel::SharedSender<ChannelMessage>> {
    /// Send a result back to the waiting reactor (glommio version).
    pub async fn send_result<R: Send + 'static>(self, result: R) -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
        let (msg_id, sender) = self.into_parts();
        sender.send((msg_id, Box::new(result)))
            .await
            .map_err(|e| Box::new(e) as Box<dyn std::error::Error + Send + Sync>)
    }
}


